// ============================================================================
// runtime.h — Core container runtime
// ============================================================================
// CONCEPT: Linux Namespaces
// ─────────────────────────
// Namespaces partition kernel resources so each container sees its own
// isolated view.  We use six of them:
//
//   CLONE_NEWPID   — PID namespace: container gets its own PID tree.
//                    First process appears as PID 1, so container init
//                    is responsible for reaping orphans.
//
//   CLONE_NEWNS    — Mount namespace: container gets its own mount table.
//                    Changes to mounts (bind-mounts, /proc, etc.) are
//                    invisible to the host and other containers.
//
//   CLONE_NEWUTS   — UTS namespace: independent hostname and NIS domain.
//                    Lets each container have its own hostname.
//
//   CLONE_NEWNET   — Network namespace: isolated network stack (interfaces,
//                    routes, iptables rules, sockets).  By default the
//                    container only sees a loopback interface.
//
//   CLONE_NEWIPC   — IPC namespace: isolated System V IPC objects and
//                    POSIX message queues.  Prevents cross-container IPC.
//
//   CLONE_NEWUSER  — User namespace: maps host UIDs to container UIDs.
//                    Enables "rootless containers" — UID 0 inside the
//                    container maps to an unprivileged host UID.
//
// CONCEPT: clone() vs fork()
// ──────────────────────────
// fork() creates an identical copy of the calling process.
// clone() is the lower-level syscall fork() calls internally, but exposes
// namespace flags directly.  We use it so the child starts inside the
// new namespaces from the very first instruction.
//
// Synchronisation protocol (parent ↔ child):
//   We use a Unix pipe pair for a simple handshake:
//     1. Parent clone()s the child.  Child blocks reading the pipe.
//     2. Parent writes UID/GID maps into /proc/<child>/uid_map & gid_map.
//        Without this the child would be "nobody" inside user namespace.
//     3. Parent writes a byte to the pipe, unblocking the child.
//     4. Child sets up mounts, hostname, optionally pivot_root.
//     5. Child drops capabilities, sets no_new_privs, then exec()s.
// ============================================================================
#pragma once

#include "container.h"
#include "cgroup.h"
#include <sys/types.h>

class Runtime {
public:
    Runtime() = default;

    // Run a container to completion.  Returns the container's exit code.
    // On fatal error (clone failed, exec failed) returns -1.
    int run(const ContainerConfig& cfg);

private:
    // ── Child-side setup ──────────────────────────────────────────────────
    // Arguments passed through the clone() boundary via a raw pointer.
    struct ChildArgs {
        const ContainerConfig* cfg;
        int pipe_read_fd;   // child reads this to know when parent is ready
    };

    // Entry point for the cloned child process.
    // Signature matches what clone() expects: int fn(void*).
    static int childEntry(void* arg);

    // Called inside childEntry — sets up the container environment.
    static void setupChild(const ContainerConfig& cfg, int pipe_read_fd);

    // Set up /proc (and optionally /dev, /sys) inside mount namespace.
    static bool mountProc();

    // Change hostname in the UTS namespace.
    static bool setHostname(const std::string& hostname);

    // Bind-mount newroot onto itself (required before pivot_root),
    // then call pivot_root(2) to make newroot the new /.
    static bool doPivotRoot(const std::string& newroot);

    // ── Parent-side setup ─────────────────────────────────────────────────
    // Write UID mapping: container UID 0 → host_uid (one entry).
    static bool writeUidMap(pid_t pid, uid_t host_uid);
    // Deny setgroups first, then write GID mapping.
    static bool writeGidMap(pid_t pid, gid_t host_gid);

    // Stack for the cloned child (clone needs an explicit stack).
    // 8 MB is generous; typical containers use far less during setup.
    static constexpr size_t STACK_SIZE = 8 * 1024 * 1024;
    char child_stack_[STACK_SIZE];
};
