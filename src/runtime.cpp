#define _GNU_SOURCE
#include "runtime.h"

#include "caps.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t Runtime::start(const ContainerConfig& cfg) {
    if (active_pid_ > 0) {
        LOG_ERROR("runtime", "cannot start a new container while one is active");
        return -1;
    }
    if (cfg.cmd.empty()) {
        LOG_ERROR("runtime", "no command specified");
        return -1;
    }

    CgroupConfig cgcfg;
    cgcfg.name               = cfg.id;
    cgcfg.memory_limit_bytes = cfg.memory_limit_bytes;
    cgcfg.cpu_quota_percent  = cfg.cpu_quota_percent;

    active_cgroup_ = std::make_unique<Cgroup>(cgcfg);
    if (!active_cgroup_->create()) {
        active_cgroup_.reset();
    }

    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) {
        LOG_ERROR("runtime", "pipe() failed: ", strerror(errno));
        if (active_cgroup_) {
            active_cgroup_->destroy();
            active_cgroup_.reset();
        }
        return -1;
    }

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    if (cfg.use_user_ns) flags |= CLONE_NEWUSER;

    ChildArgs args{&cfg, sync_pipe[0]};
    pid_t child_pid = clone(childEntry, child_stack_ + STACK_SIZE, flags, &args);
    if (child_pid < 0) {
        LOG_ERROR("runtime", "clone() failed: ", strerror(errno));
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        if (active_cgroup_) {
            active_cgroup_->destroy();
            active_cgroup_.reset();
        }
        return -1;
    }

    close(sync_pipe[0]);

    if (active_cgroup_) {
        active_cgroup_->addProcess(child_pid);
    }

    if (cfg.use_user_ns) {
        if (!writeUidMap(child_pid, cfg.host_uid)) {
            LOG_WARN("runtime", "uid_map write failed");
        }
        if (!writeGidMap(child_pid, cfg.host_gid)) {
            LOG_WARN("runtime", "gid_map write failed");
        }
    }

    char ready = 'R';
    if (write(sync_pipe[1], &ready, 1) != 1) {
        LOG_ERROR("runtime", "failed to signal child: ", strerror(errno));
    }
    close(sync_pipe[1]);

    active_pid_ = child_pid;
    return child_pid;
}

int Runtime::wait(pid_t pid) {
    int wstatus = 0;
    pid_t ret = waitpid(pid, &wstatus, 0);
    if (ret < 0) {
        LOG_ERROR("runtime", "waitpid failed: ", strerror(errno));
        finishRun(-1);
        return -1;
    }

    int exit_code = statusToExitCode(wstatus);
    finishRun(exit_code);
    return exit_code;
}

int Runtime::waitNonBlocking(pid_t pid, int& exit_code) {
    int wstatus = 0;
    pid_t ret = waitpid(pid, &wstatus, WNOHANG);
    if (ret == 0) return 0;

    if (ret < 0) {
        LOG_ERROR("runtime", "waitpid(WNOHANG) failed: ", strerror(errno));
        exit_code = -1;
        finishRun(exit_code);
        return -1;
    }

    exit_code = statusToExitCode(wstatus);
    finishRun(exit_code);
    return 1;
}

int Runtime::run(const ContainerConfig& cfg) {
    pid_t pid = start(cfg);
    if (pid < 0) return -1;
    return wait(pid);
}

int Runtime::childEntry(void* raw_arg) {
    auto* args = reinterpret_cast<ChildArgs*>(raw_arg);
    setupChild(*args->cfg, args->pipe_read_fd);
    return 127;
}

void Runtime::setupChild(const ContainerConfig& cfg, int pipe_read_fd) {
    char token = 0;
    if (read(pipe_read_fd, &token, 1) != 1) {
        LOG_WARN("runtime-child", "sync pipe read failed, proceeding");
    }
    close(pipe_read_fd);

    if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        LOG_WARN("runtime-child", "MS_PRIVATE on / failed: ", strerror(errno));
    }

    if (!mountProc()) {
        LOG_WARN("runtime-child", "failed to mount /proc");
    }

    std::string hostname = cfg.hostname.empty() ? cfg.id : cfg.hostname;
    setHostname(hostname);
    dropAllCapabilities();
    setNoNewPrivs();

    std::vector<char*> argv;
    argv.reserve(cfg.cmd.size() + 1);
    for (const auto& arg : cfg.cmd) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    LOG_INFO("runtime-child", "exec: ", cfg.cmd[0]);
    execvp(argv[0], argv.data());

    LOG_ERROR("runtime-child", "execvp(", cfg.cmd[0], ") failed: ", strerror(errno));
    _exit(127);
}

bool Runtime::mountProc() {
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr) != 0) {
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
    return true;
}

bool Runtime::writeUidMap(pid_t pid, uid_t host_uid) {
    std::ostringstream path;
    path << "/proc/" << pid << "/uid_map";
    std::ofstream file(path.str());
    if (!file.is_open()) {
        LOG_ERROR("runtime", "cannot open ", path.str(), ": ", strerror(errno));
        return false;
    }
    file << "0 " << host_uid << " 1\n";
    return file.good();
}

bool Runtime::writeGidMap(pid_t pid, gid_t host_gid) {
    {
        std::ostringstream path;
        path << "/proc/" << pid << "/setgroups";
        std::ofstream file(path.str());
        if (file.is_open()) file << "deny\n";
    }

    std::ostringstream path;
    path << "/proc/" << pid << "/gid_map";
    std::ofstream file(path.str());
    if (!file.is_open()) {
        LOG_ERROR("runtime", "cannot open ", path.str(), ": ", strerror(errno));
        return false;
    }
    file << "0 " << host_gid << " 1\n";
    return file.good();
}

int Runtime::statusToExitCode(int wstatus) {
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    if (WIFSIGNALED(wstatus)) return 128 + WTERMSIG(wstatus);
    return -1;
}

void Runtime::finishRun(int exit_code) {
    if (active_cgroup_) {
        auto stats = active_cgroup_->readStats();
        LOG_INFO("runtime",
                 "container exited, code=", exit_code,
                 " peak_mem=", stats.memory_peak_bytes,
                 "B cpu_usec=", stats.cpu_usage_usec);
        active_cgroup_->destroy();
        active_cgroup_.reset();
    } else {
        LOG_INFO("runtime", "container exited, code=", exit_code);
    }
    active_pid_ = -1;
}
