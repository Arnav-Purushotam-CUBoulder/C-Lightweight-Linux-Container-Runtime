// ============================================================================
// bench/bench.cpp — Microbenchmarks for the container runtime
// ============================================================================
// Three benchmarks, each comparing the container runtime against a native
// (no-namespace) baseline:
//
// 1. STARTUP LATENCY
//    Measures wall-clock time from clone() to the first instruction of the
//    child executable.  The child writes a timestamp into a shared pipe the
//    moment it starts (before exec()), and we compute the delta.
//    Hypothesis: namespace setup adds < 5 ms on modern kernels.
//
// 2. MEMORY OVERHEAD
//    Measures the resident-set-size (RSS) of a container running a busy
//    loop vs the same process run natively.
//    Reports: baseline RSS, container RSS, and the overhead %.
//
// 3. CPU ISOLATION
//    Spawns a CPU-intensive loop inside a container with a 25% CPU quota,
//    then measures actual CPU time consumed over a 2-second wall window.
//    Verifies the cgroup throttle is working correctly.
//    Expected: <= ~28% of one core (25% quota + a little scheduling slack).
//
// Output is newline-delimited JSON so it can be piped to jq or logged.
// ============================================================================
#define _GNU_SOURCE
#include "../src/container.h"
#include "../src/runtime.h"
#include "../src/logger.h"
#include "../src/cgroup.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

static long long nowUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1'000'000LL
         + ts.tv_nsec / 1'000LL;
}

static long long nowMs() { return nowUs() / 1000LL; }

// Print a JSON result object.
static void printResult(const std::string& bench,
                        const std::string& metric,
                        double value,
                        const std::string& unit,
                        const std::string& note = "") {
    printf("{\"bench\":\"%s\",\"metric\":\"%s\",\"value\":%.3f,"
           "\"unit\":\"%s\",\"note\":\"%s\"}\n",
           bench.c_str(), metric.c_str(), value,
           unit.c_str(), note.c_str());
    fflush(stdout);
}

// Statistics helpers.
static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0;
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}
static double minOf(const std::vector<double>& v) {
    return *std::min_element(v.begin(), v.end());
}
static double maxOf(const std::vector<double>& v) {
    return *std::max_element(v.begin(), v.end());
}
static double stddev(const std::vector<double>& v) {
    double m = mean(v);
    double sq = 0;
    for (double x : v) sq += (x - m) * (x - m);
    return std::sqrt(sq / v.size());
}

// ── Benchmark 1: Startup Latency ─────────────────────────────────────────────
// Technique: the child (both native and containerised) writes the current
// timestamp to a pipe immediately upon starting.  The parent records the
// time just before clone()/fork() and computes the delta.

static double measureNativeStartup() {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return -1;

    long long t_before = nowUs();

    pid_t pid = fork();
    if (pid == 0) {
        // Child: write start timestamp immediately.
        close(pipe_fd[0]);
        long long t_start = nowUs();
        (void)write(pipe_fd[1], &t_start, sizeof(t_start));
        close(pipe_fd[1]);
        _exit(0);
    }
    close(pipe_fd[1]);

    long long t_child;
    if (read(pipe_fd[0], &t_child, sizeof(t_child)) != sizeof(t_child)) {
        close(pipe_fd[0]);
        waitpid(pid, nullptr, 0);
        return -1;
    }
    close(pipe_fd[0]);
    waitpid(pid, nullptr, 0);

    (void)t_before;
    // Return the delta between parent's pre-fork time and child's first
    // instruction.  This captures kernel fork overhead.
    return static_cast<double>(t_child - t_before);
}

static double measureContainerStartup() {
    // We use clone() with namespace flags and measure until child writes
    // its timestamp (same technique as native but with CLONE_NEW* flags).
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return -1;

    // Simple child function — write timestamp, exit.
    struct Args { int write_fd; };
    static Args child_args;
    child_args.write_fd = pipe_fd[1];

    static constexpr size_t STACK_SZ = 1 * 1024 * 1024;
    static char stack[STACK_SZ];

    auto child_fn = [](void* arg) -> int {
        long long t = nowUs();
        auto* a = static_cast<Args*>(arg);
        (void)write(a->write_fd, &t, sizeof(t));
        close(a->write_fd);
        return 0;
    };

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD;

    long long t_before = nowUs();
    pid_t pid = clone(child_fn, stack + STACK_SZ, flags, &child_args);
    if (pid < 0) {
        close(pipe_fd[0]); close(pipe_fd[1]);
        return -1;
    }
    close(pipe_fd[1]);

    // Write UID/GID maps so child can exit cleanly.
    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "0 %d 1\n", getuid());
            (void)write(fd, buf, n); close(fd);
        }
        snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { (void)write(fd, "deny\n", 5); close(fd); }
        snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "0 %d 1\n", getgid());
            (void)write(fd, buf, n); close(fd);
        }
    }

    long long t_child = 0;
    (void)read(pipe_fd[0], &t_child, sizeof(t_child));
    close(pipe_fd[0]);
    waitpid(pid, nullptr, 0);

    return static_cast<double>(t_child - t_before);
}

static void benchStartupLatency(int iterations) {
    std::vector<double> native_us, container_us;

    for (int i = 0; i < iterations; ++i) {
        double n = measureNativeStartup();
        double c = measureContainerStartup();
        if (n > 0) native_us.push_back(n);
        if (c > 0) container_us.push_back(c);
    }

    if (native_us.empty() || container_us.empty()) {
        fprintf(stderr, "[bench] startup latency: measurement failed\n");
        return;
    }

    printResult("startup_latency", "native_mean_us",    mean(native_us),    "us");
    printResult("startup_latency", "native_min_us",     minOf(native_us),   "us");
    printResult("startup_latency", "container_mean_us", mean(container_us), "us");
    printResult("startup_latency", "container_min_us",  minOf(container_us),"us");
    printResult("startup_latency", "overhead_us",
                mean(container_us) - mean(native_us), "us",
                "namespace setup cost");
    printResult("startup_latency", "overhead_pct",
                100.0 * (mean(container_us) - mean(native_us)) / mean(native_us),
                "%");
}

// ── Benchmark 2: Memory Overhead ─────────────────────────────────────────────
// We measure the RSS of a container with a simple sleep workload vs native.
// The difference is namespace/cgroup bookkeeping overhead in kernel memory.

static long getRssKb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

static void benchMemoryOverhead(int iterations) {
    // Pipe: child writes its PID after settling (so we can read RSS).
    // We use /proc/<pid>/status VmRSS as the memory measurement.

    std::vector<double> native_rss, container_rss;

    for (int i = 0; i < iterations; ++i) {
        // --- Native baseline ---
        {
            int pipe_fd[2]; pipe(pipe_fd);
            pid_t pid = fork();
            if (pid == 0) {
                close(pipe_fd[0]);
                pid_t self = getpid();
                (void)write(pipe_fd[1], &self, sizeof(self));
                usleep(50000); // hold for 50ms so parent can read RSS
                _exit(0);
            }
            close(pipe_fd[1]);
            pid_t child_pid; (void)read(pipe_fd[0], &child_pid, sizeof(child_pid));
            close(pipe_fd[0]);
            usleep(10000); // let child settle
            long rss = getRssKb(child_pid);
            waitpid(pid, nullptr, 0);
            if (rss > 0) native_rss.push_back(static_cast<double>(rss));
        }

        // --- Container ---
        {
            int pipe_fd[2]; pipe(pipe_fd);
            struct Args { int wfd; };
            static Args ca; ca.wfd = pipe_fd[1];
            static char stk[512 * 1024];
            auto fn = [](void* arg) -> int {
                auto* a = static_cast<Args*>(arg);
                pid_t self = getpid();
                (void)write(a->wfd, &self, sizeof(self));
                usleep(50000);
                return 0;
            };
            int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                        CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD;
            pid_t pid = clone(fn, stk + sizeof(stk), flags, &ca);
            if (pid < 0) { close(pipe_fd[0]); close(pipe_fd[1]); continue; }
            close(pipe_fd[1]);
            // UID/GID maps
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
            int fd = open(path, O_WRONLY);
            if (fd >= 0) {
                char buf[32]; int n = snprintf(buf,sizeof(buf),"0 %d 1\n",getuid());
                (void)write(fd, buf, n); close(fd);
            }
            snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
            fd = open(path, O_WRONLY);
            if (fd >= 0) { (void)write(fd,"deny\n",5); close(fd); }
            snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
            fd = open(path, O_WRONLY);
            if (fd >= 0) {
                char buf[32]; int n = snprintf(buf,sizeof(buf),"0 %d 1\n",getgid());
                (void)write(fd, buf, n); close(fd);
            }
            pid_t child_pid; (void)read(pipe_fd[0], &child_pid, sizeof(child_pid));
            close(pipe_fd[0]);
            usleep(10000);
            // Note: after CLONE_NEWPID, child sees itself as PID 1 but its
            // *host* PID is what we use to read /proc/<host_pid>/status.
            long rss = getRssKb(pid); // use host PID
            waitpid(pid, nullptr, 0);
            if (rss > 0) container_rss.push_back(static_cast<double>(rss));
        }
    }

    if (native_rss.empty() || container_rss.empty()) {
        fprintf(stderr, "[bench] memory overhead: measurement failed\n");
        return;
    }

    printResult("memory_overhead", "native_rss_kb",    mean(native_rss),    "kB");
    printResult("memory_overhead", "container_rss_kb", mean(container_rss), "kB");
    printResult("memory_overhead", "overhead_kb",
                mean(container_rss) - mean(native_rss), "kB",
                "namespace kernel bookkeeping");
    printResult("memory_overhead", "stddev_container",  stddev(container_rss), "kB");
}

// ── Benchmark 3: CPU Isolation ────────────────────────────────────────────────
// Spawn a CPU-spinner in a cgroup with 25% CPU quota.
// Measure actual CPU time consumed over 2 seconds of wall time.
// Expected: <= ~28% (25% + scheduling jitter).

static void benchCpuIsolation() {
    if (!cgroupV2Available()) {
        fprintf(stderr, "[bench] cpu_isolation: cgroup v2 not available, skipping\n");
        return;
    }

    // Create a cgroup with 25% CPU quota.
    CgroupConfig cgcfg;
    cgcfg.name               = "linuc-bench-cpu";
    cgcfg.cpu_quota_percent  = 25;
    cgcfg.memory_limit_bytes = 0;
    cgcfg.max_pids           = 0;
    Cgroup cg(cgcfg);
    if (!cg.create()) {
        fprintf(stderr, "[bench] cpu_isolation: failed to create cgroup\n");
        return;
    }

    // Fork a CPU-spinner.
    pid_t pid = fork();
    if (pid == 0) {
        // Child: tight CPU loop.
        volatile long x = 0;
        while (true) { x += x * 2 + 1; }
        _exit(0);
    }

    // Move child into the cgroup.
    cg.addProcess(pid);

    // Sample CPU usage before and after 2 seconds.
    auto before = cg.readStats();
    long long wall_start = nowUs();

    usleep(2'000'000); // 2 seconds

    long long wall_elapsed_us = nowUs() - wall_start;
    auto after = cg.readStats();

    // Kill the spinner.
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    cg.destroy();

    long long cpu_used_us   = after.cpu_usage_usec - before.cpu_usage_usec;
    double    cpu_pct_actual = 100.0 * cpu_used_us / wall_elapsed_us;
    double    cpu_pct_expected = 25.0;

    printResult("cpu_isolation", "wall_time_us",      static_cast<double>(wall_elapsed_us), "us");
    printResult("cpu_isolation", "cpu_used_us",        static_cast<double>(cpu_used_us),    "us");
    printResult("cpu_isolation", "actual_cpu_pct",     cpu_pct_actual,   "%");
    printResult("cpu_isolation", "expected_cpu_pct",   cpu_pct_expected, "%");
    printResult("cpu_isolation", "throttle_effective",
                (cpu_pct_actual <= cpu_pct_expected * 1.3) ? 1.0 : 0.0,
                "bool", "1=cgroup limit respected within 30% tolerance");
}

// ── Entry point (called from main.cpp) ──────────────────────────────────────

int runBenchmarks(int iterations) {
    fprintf(stderr, "[bench] running with %d iterations per benchmark\n", iterations);

    fprintf(stderr, "[bench] === 1. Startup Latency ===\n");
    benchStartupLatency(iterations);

    fprintf(stderr, "[bench] === 2. Memory Overhead ===\n");
    benchMemoryOverhead(iterations);

    fprintf(stderr, "[bench] === 3. CPU Isolation ===\n");
    benchCpuIsolation();

    fprintf(stderr, "[bench] done\n");
    return 0;
}
