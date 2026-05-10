#pragma once

#include "container.h"

#include <string>
#include <sys/types.h>

static constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup/linuc";

class Cgroup {
public:
    explicit Cgroup(const CgroupConfig& cfg);
    ~Cgroup();

    bool create();
    bool addProcess(pid_t pid);
    bool destroy();

    struct Stats {
        int64_t memory_current_bytes = 0;
        int64_t memory_peak_bytes    = 0;
        int64_t cpu_usage_usec       = 0;
    };

    Stats readStats() const;

private:
    bool writeFile(const std::string& filename, const std::string& value) const;
    std::string readFile(const std::string& filename) const;
    bool enableControllers() const;

    CgroupConfig cfg_;
    std::string  cgroup_path_;
    bool         created_ = false;
};

bool cgroupV2Available();
