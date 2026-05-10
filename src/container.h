#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

struct CgroupConfig {
    std::string name;
    int64_t     memory_limit_bytes = 0;
    int         cpu_quota_percent  = 0;
};

struct ContainerConfig {
    std::string              id;
    std::string              hostname;
    std::vector<std::string> cmd;

    int64_t memory_limit_bytes = 0;
    int     cpu_quota_percent  = 0;

    bool use_user_ns = true;
    uid_t host_uid   = 0;
    gid_t host_gid   = 0;
};

struct SupervisorConfig {
    ContainerConfig container;
    int             max_restarts     = 0;
    int             restart_delay_ms = 500;
};

struct RunStats {
    int  exit_code     = -1;
    long wall_time_ms  = 0;
    int  restart_count = 0;
};
