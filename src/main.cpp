// ============================================================================
// main.cpp — linuc CLI entry point
// ============================================================================
// Usage:
//   linuc run   [OPTIONS] <cmd> [args...]   # run a one-shot container
//   linuc supervise [OPTIONS] <cmd> [...]   # run under supervisor
//   linuc bench [--iterations N]            # run benchmarks
//
// Common options:
//   --id <name>          Container identity (default: auto-generated)
//   --hostname <host>    Hostname inside container (default: same as --id)
//   --rootfs <path>      Root filesystem directory (default: host fs)
//   --memory <bytes>     Memory limit, e.g. 134217728 (128 MB)
//   --cpu <percent>      CPU quota 1-100 (per core), e.g. 50
//   --max-pids <n>       Max PIDs in container
//   --no-user-ns         Skip user namespace (requires root)
//   --verbose / -v       Enable DEBUG-level logging
//   --env KEY=VAL        Set environment variable (repeatable)
//
// Supervisor-only options:
//   --restart never|on-failure|always   Restart policy (default: never)
//   --max-restarts <n>                  Max restart attempts (default: 3)
// ============================================================================

#include "container.h"
#include "logger.h"
#include "runtime.h"
#include "supervisor.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// ── Helpers ───────────────────────────────────────────────────────────────────

static void printUsage(const char* prog) {
    std::cerr <<
"Usage: " << prog << R"( <subcommand> [OPTIONS] [-- <cmd> [args...]]

Subcommands:
  run        Run a container (one-shot).
  supervise  Run a container under the process supervisor.
  bench      Run built-in microbenchmarks.

Common options:
  --id <name>          Container name/ID  (default: linuc-<pid>)
  --hostname <host>    Hostname inside container
  --rootfs <dir>       Root filesystem path  (default: host /)
  --memory <bytes>     Memory limit  e.g. 134217728 = 128 MB
  --cpu <pct>          CPU quota percent per core  e.g. 50
  --max-pids <n>       Max processes in container
  --no-user-ns         Disable user namespace  (needs root)
  --env KEY=VALUE      Set env var  (repeatable)
  -v / --verbose       Enable debug logging

Supervisor options:
  --restart  never|on-failure|always   (default: never)
  --max-restarts <n>                   (default: 3)

Examples:
  sudo linuc run --memory 67108864 --cpu 50 -- /bin/sh
  sudo linuc run --rootfs /tmp/myroot -- /bin/sh
  sudo linuc supervise --restart on-failure -- /bin/my-service
  linuc bench --iterations 20
)";
}

static std::string autoId() {
    return "linuc-" + std::to_string(getpid());
}

static bool parseInt(const std::string& s, int& out) {
    try { out = std::stoi(s); return true; }
    catch (...) { return false; }
}

static bool parseInt64(const std::string& s, int64_t& out) {
    try { out = std::stoll(s); return true; }
    catch (...) { return false; }
}

// ── Argument parser ───────────────────────────────────────────────────────────

struct ParsedArgs {
    std::string              subcmd;
    ContainerConfig          container;
    SupervisorConfig         supervisor;
    bool                     verbose      = false;
    int                      bench_iters  = 10;
};

static bool parseArgs(int argc, char** argv, ParsedArgs& out) {
    if (argc < 2) {
        printUsage(argv[0]);
        return false;
    }

    out.subcmd = argv[1];
    if (out.subcmd == "-h" || out.subcmd == "--help") {
        printUsage(argv[0]);
        return false;
    }

    // Defaults
    out.container.id          = autoId();
    out.container.use_user_ns = true;
    out.container.host_uid    = getuid();
    out.container.host_gid    = getgid();
    out.supervisor.restart_policy = RestartPolicy::Never;
    out.supervisor.max_restarts   = 3;
    out.supervisor.restart_delay_ms = 500;

    int i = 2;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--") { ++i; break; }

        if ((arg == "-v" || arg == "--verbose") && i < argc) {
            out.verbose = true; ++i; continue;
        }
        if (arg == "--id" && i + 1 < argc) {
            out.container.id = argv[++i]; ++i; continue;
        }
        if (arg == "--hostname" && i + 1 < argc) {
            out.container.hostname = argv[++i]; ++i; continue;
        }
        if (arg == "--rootfs" && i + 1 < argc) {
            out.container.rootfs = argv[++i]; ++i; continue;
        }
        if (arg == "--memory" && i + 1 < argc) {
            if (!parseInt64(argv[++i], out.container.memory_limit_bytes)) {
                std::cerr << "error: --memory requires integer bytes\n";
                return false;
            }
            ++i; continue;
        }
        if (arg == "--cpu" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.container.cpu_quota_percent)) {
                std::cerr << "error: --cpu requires integer percent (1-100)\n";
                return false;
            }
            ++i; continue;
        }
        if (arg == "--max-pids" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.container.max_pids)) {
                std::cerr << "error: --max-pids requires integer\n";
                return false;
            }
            ++i; continue;
        }
        if (arg == "--no-user-ns") {
            out.container.use_user_ns = false; ++i; continue;
        }
        if (arg == "--env" && i + 1 < argc) {
            out.container.env.push_back(argv[++i]); ++i; continue;
        }
        if (arg == "--iterations" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.bench_iters)) {
                std::cerr << "error: --iterations requires integer\n";
                return false;
            }
            ++i; continue;
        }
        if (arg == "--restart" && i + 1 < argc) {
            std::string policy = argv[++i];
            if (policy == "never")       out.supervisor.restart_policy = RestartPolicy::Never;
            else if (policy == "on-failure") out.supervisor.restart_policy = RestartPolicy::OnFailure;
            else if (policy == "always") out.supervisor.restart_policy = RestartPolicy::Always;
            else { std::cerr << "error: unknown restart policy: " << policy << "\n"; return false; }
            ++i; continue;
        }
        if (arg == "--max-restarts" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.supervisor.max_restarts)) {
                std::cerr << "error: --max-restarts requires integer\n";
                return false;
            }
            ++i; continue;
        }

        // Unrecognised flag or start of command.
        break;
    }

    // Remaining argv is the command to run.
    for (; i < argc; ++i) {
        out.container.cmd.push_back(argv[i]);
    }

    // Hostname defaults to ID.
    if (out.container.hostname.empty())
        out.container.hostname = out.container.id;

    // Copy ContainerConfig into SupervisorConfig.
    out.supervisor.container = out.container;

    return true;
}

// ── Subcommand: bench (forward declaration, impl in bench/bench.cpp) ──────────
int runBenchmarks(int iterations);

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ParsedArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    if (args.verbose) {
        Logger::instance().setLevel(LogLevel::DEBUG);
    }

    // ── run ──────────────────────────────────────────────────────────────
    if (args.subcmd == "run") {
        if (args.container.cmd.empty()) {
            std::cerr << "error: no command specified after options\n";
            printUsage(argv[0]);
            return 1;
        }
        LOG_INFO("main", "starting container id=", args.container.id);
        Runtime rt;
        int code = rt.run(args.container);
        LOG_INFO("main", "container finished, exit_code=", code);
        return code;
    }

    // ── supervise ────────────────────────────────────────────────────────
    if (args.subcmd == "supervise") {
        if (args.container.cmd.empty()) {
            std::cerr << "error: no command specified after options\n";
            printUsage(argv[0]);
            return 1;
        }
        LOG_INFO("main", "starting supervisor for id=", args.container.id);
        Supervisor sup;
        int code = sup.run(args.supervisor);
        auto stats = sup.lastStats();
        LOG_INFO("main",
                 "supervisor done, code=", code,
                 " restarts=", stats.restart_count,
                 " wall_ms=",  stats.wall_time_ms);
        return code;
    }

    // ── bench ────────────────────────────────────────────────────────────
    if (args.subcmd == "bench") {
        return runBenchmarks(args.bench_iters);
    }

    std::cerr << "error: unknown subcommand '" << args.subcmd << "'\n";
    printUsage(argv[0]);
    return 1;
}
