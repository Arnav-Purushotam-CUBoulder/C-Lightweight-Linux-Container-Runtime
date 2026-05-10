#include "cgroup.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

Cgroup::Cgroup(const CgroupConfig& cfg)
    : cfg_(cfg)
    , cgroup_path_(std::string(CGROUP_ROOT) + "/" + cfg.name) {}

Cgroup::~Cgroup() = default;

bool Cgroup::create() {
    if (!cgroupV2Available()) {
        LOG_WARN("cgroup", "cgroup v2 not available - resource limits disabled");
        return false;
    }

    if (mkdir(CGROUP_ROOT, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("cgroup", "mkdir ", CGROUP_ROOT, " failed: ", strerror(errno));
        return false;
    }

    if (!enableControllers()) return false;

    if (mkdir(cgroup_path_.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("cgroup", "mkdir ", cgroup_path_, " failed: ", strerror(errno));
        return false;
    }

    if (cfg_.memory_limit_bytes > 0) {
        std::string bytes = std::to_string(cfg_.memory_limit_bytes);
        if (!writeFile("memory.max", bytes)) return false;
        writeFile("memory.swap.max", "0");
        LOG_INFO("cgroup", "memory.max=", bytes, " for ", cfg_.name);
    }

    if (cfg_.cpu_quota_percent > 0 && cfg_.cpu_quota_percent <= 100) {
        int quota_us = cfg_.cpu_quota_percent * 1000;
        std::string value = std::to_string(quota_us) + " 100000";
        if (!writeFile("cpu.max", value)) return false;
        LOG_INFO("cgroup", "cpu.max=", value, " for ", cfg_.name);
    }

    created_ = true;
    return true;
}

bool Cgroup::addProcess(pid_t pid) {
    if (!created_) return false;
    return writeFile("cgroup.procs", std::to_string(pid));
}

bool Cgroup::destroy() {
    if (!created_) return true;
    if (rmdir(cgroup_path_.c_str()) != 0) {
        LOG_WARN("cgroup", "rmdir ", cgroup_path_, " failed: ", strerror(errno));
        return false;
    }
    created_ = false;
    return true;
}

Cgroup::Stats Cgroup::readStats() const {
    Stats stats;
    if (!created_) return stats;

    std::string current = readFile("memory.current");
    if (!current.empty()) stats.memory_current_bytes = std::stoll(current);

    std::string peak = readFile("memory.peak");
    if (!peak.empty()) stats.memory_peak_bytes = std::stoll(peak);

    std::string raw = readFile("cpu.stat");
    std::istringstream input(raw);
    std::string key;
    long long value = 0;
    while (input >> key >> value) {
        if (key == "usage_usec") {
            stats.cpu_usage_usec = value;
            break;
        }
    }

    return stats;
}

bool Cgroup::writeFile(const std::string& filename, const std::string& value) const {
    std::ofstream file(cgroup_path_ + "/" + filename);
    if (!file.is_open()) {
        LOG_ERROR("cgroup", "cannot open ", cgroup_path_ + "/" + filename, ": ",
                  strerror(errno));
        return false;
    }
    file << value << "\n";
    return file.good();
}

std::string Cgroup::readFile(const std::string& filename) const {
    std::ifstream file(cgroup_path_ + "/" + filename);
    if (!file.is_open()) return "";
    std::string line;
    std::getline(file, line);
    return line;
}

bool Cgroup::enableControllers() const {
    std::ofstream file(std::string(CGROUP_ROOT) + "/cgroup.subtree_control");
    if (!file.is_open()) {
        std::ofstream root("/sys/fs/cgroup/cgroup.subtree_control");
        if (!root.is_open()) {
            LOG_WARN("cgroup", "cannot open cgroup.subtree_control - run as root");
            return false;
        }
        root << "+cpu +memory\n";

        std::ofstream retry(std::string(CGROUP_ROOT) + "/cgroup.subtree_control");
        if (!retry.is_open()) return false;
        retry << "+cpu +memory\n";
        return retry.good();
    }

    file << "+cpu +memory\n";
    return file.good();
}

bool cgroupV2Available() {
    struct statfs fs {};
    if (statfs("/sys/fs/cgroup", &fs) != 0) return false;
    return fs.f_type == static_cast<long>(CGROUP2_SUPER_MAGIC);
}
