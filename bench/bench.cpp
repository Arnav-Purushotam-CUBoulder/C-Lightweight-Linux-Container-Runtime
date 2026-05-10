#define _GNU_SOURCE
#include "../src/cgroup.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr int kIterations = 10;

static long long nowUs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1'000'000LL + ts.tv_nsec / 1'000LL;
}

static void printResult(const std::string& bench,
                        const std::string& metric,
                        double value,
                        const std::string& unit,
                        const std::string& note = "") {
    printf("{\"bench\":\"%s\",\"metric\":\"%s\",\"value\":%.3f,"
           "\"unit\":\"%s\",\"note\":\"%s\"}\n",
           bench.c_str(), metric.c_str(), value, unit.c_str(), note.c_str());
}

static double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

static void writeIdMaps(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "0 %d 1\n", getuid());
        (void)write(fd, buf, n);
        close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "deny\n", 5);
        close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "0 %d 1\n", getgid());
        (void)write(fd, buf, n);
        close(fd);
    }
}

static double measureNativeStartup() {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return -1;

    long long start = nowUs();
    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fd[0]);
        long long child_start = nowUs();
        (void)write(pipe_fd[1], &child_start, sizeof(child_start));
        close(pipe_fd[1]);
        _exit(0);
    }

    close(pipe_fd[1]);
    long long child_start = 0;
    if (read(pipe_fd[0], &child_start, sizeof(child_start)) != sizeof(child_start)) {
        close(pipe_fd[0]);
        waitpid(pid, nullptr, 0);
        return -1;
    }
    close(pipe_fd[0]);
    waitpid(pid, nullptr, 0);
    return static_cast<double>(child_start - start);
}

static double measureContainerStartup() {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return -1;

    struct Args { int write_fd; };
    static Args args;
    args.write_fd = pipe_fd[1];

    static constexpr size_t kStackSize = 1 * 1024 * 1024;
    static char stack[kStackSize];

    auto child = [](void* raw) -> int {
        auto* args = static_cast<Args*>(raw);
        long long child_start = nowUs();
        (void)write(args->write_fd, &child_start, sizeof(child_start));
        close(args->write_fd);
        return 0;
    };

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD;

    long long start = nowUs();
    pid_t pid = clone(child, stack + kStackSize, flags, &args);
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    close(pipe_fd[1]);
    writeIdMaps(pid);

    long long child_start = 0;
    (void)read(pipe_fd[0], &child_start, sizeof(child_start));
    close(pipe_fd[0]);
    waitpid(pid, nullptr, 0);
    return static_cast<double>(child_start - start);
}

static void benchStartupLatency() {
    std::vector<double> native_us;
    std::vector<double> container_us;

    for (int i = 0; i < kIterations; ++i) {
        double native = measureNativeStartup();
        double container = measureContainerStartup();
        if (native > 0) native_us.push_back(native);
        if (container > 0) container_us.push_back(container);
    }

    if (native_us.empty() || container_us.empty()) return;

    printResult("startup_latency", "native_mean_us", mean(native_us), "us");
    printResult("startup_latency", "container_mean_us", mean(container_us), "us");
    printResult("startup_latency", "overhead_us",
                mean(container_us) - mean(native_us), "us",
                "namespace setup cost");
}

static long getRssKb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* file = fopen(path, "r");
    if (!file) return -1;

    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %ld", &rss);
            break;
        }
    }

    fclose(file);
    return rss;
}

static void benchMemoryOverhead() {
    std::vector<double> native_rss;
    std::vector<double> container_rss;

    for (int i = 0; i < kIterations; ++i) {
        {
            int pipe_fd[2];
            pipe(pipe_fd);
            pid_t pid = fork();
            if (pid == 0) {
                close(pipe_fd[0]);
                pid_t self = getpid();
                (void)write(pipe_fd[1], &self, sizeof(self));
                usleep(50000);
                _exit(0);
            }

            close(pipe_fd[1]);
            pid_t child_pid = 0;
            (void)read(pipe_fd[0], &child_pid, sizeof(child_pid));
            close(pipe_fd[0]);
            usleep(10000);

            long rss = getRssKb(child_pid);
            waitpid(pid, nullptr, 0);
            if (rss > 0) native_rss.push_back(static_cast<double>(rss));
        }

        {
            int pipe_fd[2];
            pipe(pipe_fd);

            struct Args { int write_fd; };
            static Args args;
            args.write_fd = pipe_fd[1];

            static char stack[512 * 1024];
            auto child = [](void* raw) -> int {
                auto* args = static_cast<Args*>(raw);
                pid_t self = getpid();
                (void)write(args->write_fd, &self, sizeof(self));
                usleep(50000);
                return 0;
            };

            int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                        CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD;

            pid_t pid = clone(child, stack + sizeof(stack), flags, &args);
            if (pid < 0) {
                close(pipe_fd[0]);
                close(pipe_fd[1]);
                continue;
            }

            close(pipe_fd[1]);
            writeIdMaps(pid);

            pid_t ignored_child_pid = 0;
            (void)read(pipe_fd[0], &ignored_child_pid, sizeof(ignored_child_pid));
            close(pipe_fd[0]);
            usleep(10000);

            long rss = getRssKb(pid);
            waitpid(pid, nullptr, 0);
            if (rss > 0) container_rss.push_back(static_cast<double>(rss));
        }
    }

    if (native_rss.empty() || container_rss.empty()) return;

    printResult("memory_overhead", "native_rss_kb", mean(native_rss), "kB");
    printResult("memory_overhead", "container_rss_kb", mean(container_rss), "kB");
    printResult("memory_overhead", "overhead_kb",
                mean(container_rss) - mean(native_rss), "kB");
}

static void benchCpuIsolation() {
    if (!cgroupV2Available()) return;

    CgroupConfig cgcfg;
    cgcfg.name = "linuc-bench-cpu";
    cgcfg.cpu_quota_percent = 25;

    Cgroup cgroup(cgcfg);
    if (!cgroup.create()) return;

    pid_t pid = fork();
    if (pid == 0) {
        volatile long x = 0;
        while (true) x += x * 2 + 1;
        _exit(0);
    }

    cgroup.addProcess(pid);
    auto before = cgroup.readStats();
    long long wall_start = nowUs();
    usleep(2'000'000);
    long long wall_elapsed = nowUs() - wall_start;
    auto after = cgroup.readStats();

    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    cgroup.destroy();

    long long cpu_used = after.cpu_usage_usec - before.cpu_usage_usec;
    double actual_cpu_pct = 100.0 * cpu_used / wall_elapsed;

    printResult("cpu_isolation", "actual_cpu_pct", actual_cpu_pct, "%");
    printResult("cpu_isolation", "expected_cpu_pct", 25.0, "%");
    printResult("cpu_isolation", "throttle_effective",
                actual_cpu_pct <= 32.5 ? 1.0 : 0.0, "bool",
                "1=cgroup limit respected");
}

int runBenchmarks() {
    fprintf(stderr, "[bench] running with %d iterations per benchmark\n", kIterations);
    benchStartupLatency();
    benchMemoryOverhead();
    benchCpuIsolation();
    return 0;
}
