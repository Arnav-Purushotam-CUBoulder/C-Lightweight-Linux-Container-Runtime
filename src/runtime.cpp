// ============================================================================
// runtime.cpp — Core container runtime implementation
// ============================================================================
#define _GNU_SOURCE
#include "runtime.h"
#include "caps.h"
#include "cgroup.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <grp.h>
#include <sched.h>        // clone(), CLONE_NEW*
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// pivot_root is not wrapped by glibc — call it through syscall(2).
static int pivot_root(const char* new_root, const char* put_old) {
    return static_cast<int>(syscall(SYS_pivot_root, new_root, put_old));
}

// ── Public ───────────────────────────────────────────────────────────────────

int Runtime::run(const ContainerConfig& cfg) {
    if (cfg.cmd.empty()) {
        LOG_ERROR("runtime", "no command specified");
        return -1;
    }

    // ── 1. Set up cgroup BEFORE cloning ──────────────────────────────────
    // We create the cgroup first so we can move the child PID into it
    // immediately after clone().  Limits are active before the workload runs.
    CgroupConfig cgcfg;
    cgcfg.name                = cfg.id;
    cgcfg.memory_limit_bytes  = cfg.memory_limit_bytes;
    cgcfg.cpu_quota_percent   = cfg.cpu_quota_percent;
    cgcfg.max_pids            = cfg.max_pids;

    Cgroup cgroup(cgcfg);
    bool has_cgroup = cgroup.create();  // non-fatal if not root

    // ── 2. Pipe for parent→child synchronisation ──────────────────────────
    // Parent writes UID/GID maps before signalling child to proceed.
    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) {
        LOG_ERROR("runtime", "pipe() failed: ", strerror(errno));
        return -1;
    }

    // ── 3. Assemble namespace flags ───────────────────────────────────────
    int ns_flags = CLONE_NEWPID   // Isolated PID tree
                 | CLONE_NEWNS    // Isolated mount table
                 | CLONE_NEWUTS   // Isolated hostname
                 | CLONE_NEWIPC   // Isolated IPC objects
                 | CLONE_NEWNET   // Isolated network stack
                 | SIGCHLD;       // Deliver SIGCHLD to parent on child exit

    if (cfg.use_user_ns) {
        ns_flags |= CLONE_NEWUSER; // Rootless: UID mapping
    }

    // ── 4. Clone the child ────────────────────────────────────────────────
    // clone() is like fork() but starts the child in a new function and
    // accepts an explicit stack pointer (stacks grow downward on x86-64,
    // so we pass the TOP of the buffer).
    ChildArgs args{&cfg, sync_pipe[0]};

    LOG_INFO("runtime", "cloning child with namespaces: pid|ns|uts|ipc|net",
             (cfg.use_user_ns ? "|user" : ""));

    pid_t child_pid = clone(childEntry,
                            child_stack_ + STACK_SIZE,
                            ns_flags,
                            &args);
    if (child_pid < 0) {
        LOG_ERROR("runtime", "clone() failed: ", strerror(errno));
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return -1;
    }
    LOG_INFO("runtime", "child cloned, host-pid=", child_pid);

    // Close child's end in the parent.
    close(sync_pipe[0]);

    // ── 5. Parent: configure namespaces before unblocking child ──────────

    // 5a. Move child into cgroup (resource limits apply immediately).
    if (has_cgroup) {
        cgroup.addProcess(child_pid);
    }

    // 5b. Write UID/GID maps so the child can see itself as UID 0 inside
    //     the user namespace.  Must happen before child proceeds.
    if (cfg.use_user_ns) {
        if (!writeUidMap(child_pid, cfg.host_uid)) {
            LOG_WARN("runtime", "uid_map write failed — container may lack capabilities");
        }
        if (!writeGidMap(child_pid, cfg.host_gid)) {
            LOG_WARN("runtime", "gid_map write failed");
        }
    }

    // 5c. Signal child: write one byte so child unblocks from read().
    char ready = 'R';
    if (write(sync_pipe[1], &ready, 1) != 1) {
        LOG_ERROR("runtime", "failed to signal child: ", strerror(errno));
    }
    close(sync_pipe[1]);

    // ── 6. Wait for container to exit ─────────────────────────────────────
    int wstatus = 0;
    pid_t ret = waitpid(child_pid, &wstatus, 0);
    if (ret < 0) {
        LOG_ERROR("runtime", "waitpid failed: ", strerror(errno));
        cgroup.destroy();
        return -1;
    }

    int exit_code = WIFEXITED(wstatus)   ? WEXITSTATUS(wstatus)
                  : WIFSIGNALED(wstatus) ? (128 + WTERMSIG(wstatus))
                  : -1;

    // Log cgroup stats before destroying.
    if (has_cgroup) {
        auto stats = cgroup.readStats();
        LOG_INFO("runtime",
                 "container exited, code=", exit_code,
                 " peak_mem=", stats.memory_peak_bytes,
                 "B cpu_usec=", stats.cpu_usage_usec);
        cgroup.destroy();
    } else {
        LOG_INFO("runtime", "container exited, code=", exit_code);
    }

    return exit_code;
}

// ── Child-side implementation ─────────────────────────────────────────────

int Runtime::childEntry(void* raw_arg) {
    auto* args = reinterpret_cast<ChildArgs*>(raw_arg);
    setupChild(*args->cfg, args->pipe_read_fd);
    // setupChild calls execvp(); if we reach here exec failed.
    return 127;
}

void Runtime::setupChild(const ContainerConfig& cfg, int pipe_read_fd) {
    // ── A. Wait for parent to write UID/GID maps ─────────────────────────
    // We MUST block here.  If we call exec() before the parent writes
    // uid_map, we'd run with no capabilities inside the user namespace.
    char buf;
    if (read(pipe_read_fd, &buf, 1) != 1) {
        // Parent may have died; proceed anyway (caps will be limited).
        LOG_WARN("runtime-child", "sync pipe read failed, proceeding");
    }
    close(pipe_read_fd);
    LOG_DEBUG("runtime-child", "received parent sync signal");

    // ── B. Remount the mount namespace as private ─────────────────────────
    // By default the child inherits the parent's mount namespace with
    // MS_SHARED propagation.  We flip it to MS_PRIVATE so our bind-mounts
    // don't leak back to the host.
    if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        LOG_WARN("runtime-child", "MS_PRIVATE on / failed: ", strerror(errno));
    }

    // ── C. Pivot root (if a rootfs path was given) ────────────────────────
    if (!cfg.rootfs.empty()) {
        if (!doPivotRoot(cfg.rootfs)) {
            LOG_ERROR("runtime-child", "pivot_root failed — falling back to host fs");
        }
    }

    // ── D. Mount /proc ────────────────────────────────────────────────────
    // /proc is PID-namespace-aware.  Mounting it inside the container makes
    // tools like `ps`, `top`, `/proc/self` work correctly and show only
    // the container's process tree.
    if (!mountProc()) {
        LOG_WARN("runtime-child", "failed to mount /proc (ps/top may not work)");
    }

    // ── E. Set hostname in UTS namespace ──────────────────────────────────
    std::string hostname = cfg.hostname.empty() ? cfg.id : cfg.hostname;
    setHostname(hostname);

    // ── F. Drop capabilities ──────────────────────────────────────────────
    // At this point we no longer need any capabilities — the filesystem and
    // namespaces are configured.  Drop everything to minimise attack surface.
    dropAllCapabilities();
    setNoNewPrivs();
    LOG_DEBUG("runtime-child", "caps after drop: ", currentCapsString());

    // ── G. Build argv / envp for exec ─────────────────────────────────────
    std::vector<char*> argv;
    argv.reserve(cfg.cmd.size() + 1);
    for (const auto& s : cfg.cmd) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<char*> envp;
    if (cfg.env.empty()) {
        // Inherit parent environment if none specified.
        extern char** environ;
        for (char** e = environ; *e; ++e) envp.push_back(*e);
    } else {
        envp.reserve(cfg.env.size() + 1);
        for (const auto& s : cfg.env) {
            envp.push_back(const_cast<char*>(s.c_str()));
        }
    }
    envp.push_back(nullptr);

    // ── H. Exec the workload ──────────────────────────────────────────────
    LOG_INFO("runtime-child", "exec: ", cfg.cmd[0]);
    execvpe(argv[0], argv.data(), envp.data());

    // If we get here, exec failed.
    LOG_ERROR("runtime-child", "execvpe(", cfg.cmd[0], ") failed: ", strerror(errno));
    _exit(127);
}

bool Runtime::mountProc() {
    // Create /proc if it doesn't exist in the new rootfs.
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc",
              MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr) != 0) {
        LOG_WARN("runtime-child", "mount proc failed: ", strerror(errno));
        return false;
    }
    return true;
}

bool Runtime::setHostname(const std::string& hostname) {
    if (sethostname(hostname.c_str(), hostname.size()) != 0) {
        LOG_WARN("runtime-child", "sethostname failed: ", strerror(errno));
        return false;
    }
    LOG_DEBUG("runtime-child", "hostname set to: ", hostname);
    return true;
}

bool Runtime::doPivotRoot(const std::string& newroot) {
    // pivot_root requirements:
    //   1. new_root must be a mount point (bind-mount it onto itself).
    //   2. Both new_root and put_old must be on the same filesystem.
    //   3. put_old must be under new_root.

    // Step 1: bind-mount newroot onto itself to make it a mount point.
    if (mount(newroot.c_str(), newroot.c_str(), nullptr,
              MS_BIND | MS_REC, nullptr) != 0) {
        LOG_ERROR("runtime-child", "bind-mount ", newroot, " failed: ", strerror(errno));
        return false;
    }

    // Step 2: create put_old directory inside new_root.
    std::string put_old = newroot + "/.old_root";
    mkdir(put_old.c_str(), 0700);

    // Step 3: pivot_root atomically swaps / with newroot.
    if (pivot_root(newroot.c_str(), put_old.c_str()) != 0) {
        LOG_ERROR("runtime-child", "pivot_root failed: ", strerror(errno));
        return false;
    }

    // Step 4: fix CWD — after pivot_root our cwd may point into old root.
    chdir("/");

    // Step 5: unmount and remove the old root.
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        LOG_WARN("runtime-child", "umount old_root failed: ", strerror(errno));
    }
    rmdir("/.old_root");

    LOG_INFO("runtime-child", "pivot_root succeeded, new root: ", newroot);
    return true;
}

// ── Parent helpers for UID/GID maps ─────────────────────────────────────────
// These run in the parent process, writing into /proc/<child_pid>/.

bool Runtime::writeUidMap(pid_t pid, uid_t host_uid) {
    // Format: "<container-uid> <host-uid> <count>\n"
    // We map exactly one UID: container UID 0 → caller's host UID.
    std::ostringstream path;
    path << "/proc/" << pid << "/uid_map";
    std::ofstream f(path.str());
    if (!f.is_open()) {
        LOG_ERROR("runtime", "cannot open ", path.str(), ": ", strerror(errno));
        return false;
    }
    f << "0 " << host_uid << " 1\n";
    return f.good();
}

bool Runtime::writeGidMap(pid_t pid, gid_t host_gid) {
    // Must write "deny" to setgroups before writing gid_map when not root.
    {
        std::ostringstream sg_path;
        sg_path << "/proc/" << pid << "/setgroups";
        std::ofstream sg(sg_path.str());
        if (sg.is_open()) sg << "deny\n";
    }

    std::ostringstream path;
    path << "/proc/" << pid << "/gid_map";
    std::ofstream f(path.str());
    if (!f.is_open()) {
        LOG_ERROR("runtime", "cannot open ", path.str(), ": ", strerror(errno));
        return false;
    }
    f << "0 " << host_gid << " 1\n";
    return f.good();
}
