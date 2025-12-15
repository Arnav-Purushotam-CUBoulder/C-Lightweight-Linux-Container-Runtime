// ============================================================================
// cgroup.h — cgroup v2 resource controller
// ============================================================================
// CONCEPT: cgroups (control groups)
// ─────────────────────────────────
// cgroups are a Linux kernel mechanism that organises processes into a
// hierarchy and lets the kernel enforce resource quotas on each group.
// Every process belongs to exactly one cgroup at any time.
//
// cgroup v2 uses a single unified hierarchy mounted at /sys/fs/cgroup.
// Each container gets its own subdirectory, e.g.:
//   /sys/fs/cgroup/linuc/<container-id>/
//
// Key files we write:
//   cgroup.procs      — move a PID into this cgroup (one PID per write)
//   memory.max        — hard memory limit in bytes ("max" = unlimited)
//   memory.swap.max   — disable swap inside the container
//   cpu.max           — CPU bandwidth: "<quota_us> <period_us>"
//                       e.g. "50000 100000" caps usage at 50% of one core
//   pids.max          — max number of processes/threads in the cgroup
//
// Key files we read (for stats):
//   memory.current    — bytes currently in use
//   memory.peak       — high-water mark since cgroup creation
//   cpu.stat          — cumulative CPU usage in microseconds
//   pids.current      — current process count
// ============================================================================
#pragma once

#include "container.h"
#include <string>
#include <sys/types.h>

// Root under which we create per-container sub-cgroups.
static constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup/linuc";

class Cgroup {
public:
    explicit Cgroup(const CgroupConfig& cfg);
    ~Cgroup();

    // Create the cgroup directory and write resource limits.
    // Returns false and logs on failure.
    bool create();

    // Move a process into this cgroup by writing its PID to cgroup.procs.
    bool addProcess(pid_t pid);

    // Remove the cgroup directory (must be empty of processes first).
    bool destroy();

    bool isCreated() const { return created_; }
    std::string path() const { return cgroup_path_; }

    // ── Stats ────────────────────────────────────────────────────────────
    struct Stats {
        int64_t memory_current_bytes = 0;
        int64_t memory_peak_bytes    = 0;
        int64_t cpu_usage_usec       = 0;
        int     pids_current         = 0;
    };
    Stats readStats() const;

private:
    bool writeFile(const std::string& filename, const std::string& value) const;
    std::string readFile(const std::string& filename) const;
    bool enableControllers() const;   // echo "+cpu +memory +pids" to parent

    CgroupConfig cfg_;
    std::string  cgroup_path_;
    bool         created_ = false;
};

// Detect which cgroup version is active on this system.
bool cgroupV2Available();
