// ============================================================================
// container.h — Shared configuration types
// ============================================================================
// Plain-old-data structs that flow from the CLI through every subsystem.
// No business logic here — just the data shapes that represent a container
// request and its resource policy.
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

// ---------------------------------------------------------------------------
// CgroupConfig — Resource limits for a single container's cgroup.
// ---------------------------------------------------------------------------
// We target the cgroup v2 unified hierarchy (available on Linux ≥ 4.5,
// default in Ubuntu 22.04+).  All zero/empty values mean "unlimited".
struct CgroupConfig {
    std::string name;               // Unique container ID (used as dir name)
    int64_t     memory_limit_bytes; // 0 = no limit  (writes to memory.max)
    int         cpu_quota_percent;  // 0 = no limit  (writes to cpu.max)
                                    //   50 → "50000 100000" (50% of one core)
    int         max_pids;           // 0 = no limit  (writes to pids.max)
};

// ---------------------------------------------------------------------------
// ContainerConfig — Everything needed to start one container.
// ---------------------------------------------------------------------------
struct ContainerConfig {
    // Identity
    std::string id;                 // Container ID (auto-generated if empty)
    std::string hostname;           // UTS hostname inside container

    // Filesystem
    // If rootfs is empty the container shares the host's mount namespace root
    // (useful for quick testing).  If set, we pivot_root into this directory.
    std::string rootfs;

    // Workload
    std::vector<std::string> cmd;   // argv — cmd[0] is the executable path
    std::vector<std::string> env;   // Environment variables ("KEY=VALUE")

    // Resource limits (passed straight to CgroupConfig)
    int64_t memory_limit_bytes = 0; // e.g. 128 * 1024 * 1024 for 128 MB
    int     cpu_quota_percent  = 0; // 0–100 (per single CPU core)
    int     max_pids           = 0;

    // Namespace options
    bool use_user_ns = true;  // Enable user namespace (allows rootless use)

    // UID/GID mapping for user namespace:
    //   Inside the container, processes appear to run as host_uid → 0 (root).
    uid_t host_uid = 0;       // Host UID to map to container UID 0
    gid_t host_gid = 0;       // Host GID to map to container GID 0
};

// ---------------------------------------------------------------------------
// SupervisorConfig — Supervisor-layer policy on top of ContainerConfig.
// ---------------------------------------------------------------------------
enum class RestartPolicy {
    Never,      // Don't restart (one-shot)
    OnFailure,  // Restart only on non-zero exit
    Always,     // Always restart
};

struct SupervisorConfig {
    ContainerConfig container;
    RestartPolicy   restart_policy = RestartPolicy::Never;
    int             max_restarts   = 3;    // Ignored when policy == Never
    int             restart_delay_ms = 500; // Wait between restarts
};

// ---------------------------------------------------------------------------
// RunStats — Filled in after a container run completes.
// ---------------------------------------------------------------------------
struct RunStats {
    int      exit_code       = -1;
    long     wall_time_ms    = 0;   // Wall-clock duration
    long     startup_time_us = 0;   // Latency from clone() to first exec()
    int64_t  peak_memory_bytes = 0; // Read from memory.peak in cgroup v2
    int      restart_count   = 0;
};
