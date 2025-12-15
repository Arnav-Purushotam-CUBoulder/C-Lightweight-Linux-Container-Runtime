// ============================================================================
// cgroup.cpp — cgroup v2 resource controller implementation
// ============================================================================
#include "cgroup.h"
#include "logger.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/vfs.h>      // statfs, CGROUP2_SUPER_MAGIC
#include <unistd.h>

// cgroup2 magic number (from <linux/magic.h>)
#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

// ── Public ──────────────────────────────────────────────────────────────────

Cgroup::Cgroup(const CgroupConfig& cfg)
    : cfg_(cfg)
    , cgroup_path_(std::string(CGROUP_ROOT) + "/" + cfg.name)
{}

Cgroup::~Cgroup() {
    // Destructor does NOT auto-destroy — callers must call destroy() explicitly
    // so we don't inadvertently kill cgroups that still have live processes.
}

bool Cgroup::create() {
    if (!cgroupV2Available()) {
        LOG_WARN("cgroup", "cgroup v2 not available — resource limits disabled");
        return false;
    }

    // 1. Ensure the linuc parent directory exists.
    if (mkdir(CGROUP_ROOT, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("cgroup", "mkdir ", CGROUP_ROOT, " failed: ", strerror(errno));
        return false;
    }

    // 2. Enable controllers on the linuc parent so child cgroups inherit them.
    //    Without this, cpu/memory/pids controllers won't be available in the
    //    container's sub-cgroup.
    if (!enableControllers()) return false;

    // 3. Create the per-container sub-directory.
    if (mkdir(cgroup_path_.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("cgroup", "mkdir ", cgroup_path_, " failed: ", strerror(errno));
        return false;
    }

    // 4. Write resource limits ────────────────────────────────────────────

    // Memory limit — "max" means unlimited.
    if (cfg_.memory_limit_bytes > 0) {
        std::string val = std::to_string(cfg_.memory_limit_bytes);
        if (!writeFile("memory.max", val)) return false;
        // Disable swap entirely so the process can't escape memory limits.
        writeFile("memory.swap.max", "0");
        LOG_INFO("cgroup", "memory.max=", val, " bytes for ", cfg_.name);
    }

    // CPU quota — expressed as "quota_us period_us".
    // We use a 100 ms period (100000 µs) which is the kernel default.
    // A 50% quota means the cgroup can use at most 50 ms per 100 ms period.
    if (cfg_.cpu_quota_percent > 0 && cfg_.cpu_quota_percent <= 100) {
        int quota_us = (cfg_.cpu_quota_percent * 100000) / 100;
        std::string val = std::to_string(quota_us) + " 100000";
        if (!writeFile("cpu.max", val)) return false;
        LOG_INFO("cgroup", "cpu.max=", val, " (", cfg_.cpu_quota_percent, "%) for ", cfg_.name);
    }

    // PID limit — "max" means unlimited.
    if (cfg_.max_pids > 0) {
        if (!writeFile("pids.max", std::to_string(cfg_.max_pids))) return false;
        LOG_INFO("cgroup", "pids.max=", cfg_.max_pids, " for ", cfg_.name);
    }

    created_ = true;
    LOG_INFO("cgroup", "created cgroup at ", cgroup_path_);
    return true;
}

bool Cgroup::addProcess(pid_t pid) {
    if (!created_) {
        LOG_WARN("cgroup", "addProcess called before create()");
        return false;
    }
    bool ok = writeFile("cgroup.procs", std::to_string(pid));
    if (ok) {
        LOG_DEBUG("cgroup", "moved pid=", pid, " into ", cgroup_path_);
    } else {
        LOG_ERROR("cgroup", "failed to move pid=", pid, " into cgroup: ",
                  strerror(errno));
    }
    return ok;
}

bool Cgroup::destroy() {
    if (!created_) return true;

    // The kernel refuses to rmdir a cgroup that still contains processes.
    // Callers must ensure the container has exited first.
    if (rmdir(cgroup_path_.c_str()) != 0) {
        LOG_WARN("cgroup", "rmdir ", cgroup_path_, " failed: ", strerror(errno),
                 " (processes still running?)");
        return false;
    }
    created_ = false;
    LOG_DEBUG("cgroup", "removed cgroup ", cgroup_path_);
    return true;
}

Cgroup::Stats Cgroup::readStats() const {
    Stats s;
    if (!created_) return s;

    // memory.current — present usage
    {
        std::string v = readFile("memory.current");
        if (!v.empty()) s.memory_current_bytes = std::stoll(v);
    }
    // memory.peak — high-water mark (available in kernel ≥ 5.19)
    {
        std::string v = readFile("memory.peak");
        if (!v.empty()) s.memory_peak_bytes = std::stoll(v);
    }
    // cpu.stat — parse "usage_usec <n>" line
    {
        std::string raw = readFile("cpu.stat");
        std::istringstream ss(raw);
        std::string key; long long val;
        while (ss >> key >> val) {
            if (key == "usage_usec") { s.cpu_usage_usec = val; break; }
        }
    }
    // pids.current
    {
        std::string v = readFile("pids.current");
        if (!v.empty()) s.pids_current = std::stoi(v);
    }
    return s;
}

// ── Private ─────────────────────────────────────────────────────────────────

bool Cgroup::enableControllers() const {
    // Write "+cpu +memory +pids" to the linuc parent's subtree_control.
    // This propagates available controllers down to child cgroups.
    std::string ctrl_path = std::string(CGROUP_ROOT) + "/cgroup.subtree_control";
    std::ofstream f(ctrl_path);
    if (!f.is_open()) {
        // If we can't open it, try the root cgroup first.
        std::ofstream root_ctrl("/sys/fs/cgroup/cgroup.subtree_control");
        if (!root_ctrl.is_open()) {
            LOG_WARN("cgroup", "cannot open cgroup.subtree_control — "
                               "run as root or delegate cgroups");
            return false;
        }
        root_ctrl << "+cpu +memory +pids\n";
        // Now try the linuc subdir again.
        std::ofstream f2(ctrl_path);
        if (!f2.is_open()) return false; // linuc dir might not exist yet; OK
        f2 << "+cpu +memory +pids\n";
    } else {
        f << "+cpu +memory +pids\n";
    }
    return true;
}

bool Cgroup::writeFile(const std::string& filename, const std::string& value) const {
    std::string fullpath = cgroup_path_ + "/" + filename;
    std::ofstream f(fullpath);
    if (!f.is_open()) {
        LOG_ERROR("cgroup", "cannot open ", fullpath, ": ", strerror(errno));
        return false;
    }
    f << value << "\n";
    return f.good();
}

std::string Cgroup::readFile(const std::string& filename) const {
    std::string fullpath = cgroup_path_ + "/" + filename;
    std::ifstream f(fullpath);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    return line;
}

// ── Free functions ───────────────────────────────────────────────────────────

bool cgroupV2Available() {
    struct statfs fs{};
    if (statfs("/sys/fs/cgroup", &fs) != 0) return false;
    return fs.f_type == static_cast<long>(CGROUP2_SUPER_MAGIC);
}
