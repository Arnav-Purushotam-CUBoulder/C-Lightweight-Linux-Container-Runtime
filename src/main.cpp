#include "container.h"
#include "logger.h"
#include "runtime.h"
#include "supervisor.h"

#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

static void printUsage(const char* prog) {
    std::cerr <<
"Usage: " << prog << R"( <subcommand> [OPTIONS] [-- <cmd> [args...]]

Subcommands:
  run        Run one containerised command.
  supervise  Restart a command when it exits non-zero.
  bench      Run the built-in benchmarks.

Common options:
  --id <name>          Container name/ID  (default: linuc-<pid>)
  --hostname <host>    Hostname inside container
  --memory <bytes>     Memory limit in bytes
  --cpu <pct>          CPU quota percent per core
  --no-user-ns         Disable user namespace  (needs root)
  -v / --verbose       Enable debug logging

Supervisor options:
  --max-restarts <n>   Restart on failure up to N times

Examples:
  sudo linuc run --memory 67108864 --cpu 50 -- /bin/sh
  sudo linuc supervise --max-restarts 3 -- /bin/my-service
  sudo linuc bench
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

struct ParsedArgs {
    std::string      subcmd;
    ContainerConfig  container;
    SupervisorConfig supervisor;
    bool             verbose = false;
};

static bool parseArgs(int argc, char** argv, ParsedArgs& out) {  // argv holds the CLI strings
    if (argc < 2) {
        printUsage(argv[0]);
        return false;
    }

    out.subcmd = argv[1];
    if (out.subcmd == "-h" || out.subcmd == "--help") {
        printUsage(argv[0]);
        return false;
    }

    out.container.id = autoId();
    out.container.use_user_ns = true;
    out.container.host_uid = getuid();
    out.container.host_gid = getgid();

    int i = 2;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "--") { ++i; break; }

        if (arg == "-v" || arg == "--verbose") {
            out.verbose = true;
            ++i;
            continue;
        }
        if (arg == "--id" && i + 1 < argc) {
            out.container.id = argv[++i];
            ++i;
            continue;
        }
        if (arg == "--hostname" && i + 1 < argc) {
            out.container.hostname = argv[++i];
            ++i;
            continue;
        }
        if (arg == "--memory" && i + 1 < argc) {
            if (!parseInt64(argv[++i], out.container.memory_limit_bytes)) {
                std::cerr << "error: --memory requires integer bytes\n";
                return false;
            }
            ++i;
            continue;
        }
        if (arg == "--cpu" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.container.cpu_quota_percent)) {
                std::cerr << "error: --cpu requires integer percent\n";
                return false;
            }
            ++i;
            continue;
        }
        if (arg == "--max-restarts" && i + 1 < argc) {
            if (!parseInt(argv[++i], out.supervisor.max_restarts)) {
                std::cerr << "error: --max-restarts requires integer\n";
                return false;
            }
            ++i;
            continue;
        }
        if (arg == "--no-user-ns") {
            out.container.use_user_ns = false;
            ++i;
            continue;
        }
        break;
    }

    for (; i < argc; ++i) {
        out.container.cmd.push_back(argv[i]);
    }

    if (out.container.hostname.empty()) {
        out.container.hostname = out.container.id;
    }
    out.supervisor.container = out.container;
    return true;
}

int runBenchmarks();

int main(int argc, char** argv) {  // argc = count, argv = array of C-string arguments
    ParsedArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    if (args.verbose) {
        Logger::instance().setLevel(LogLevel::DEBUG);
    }

    if (args.subcmd == "run") {
        if (args.container.cmd.empty()) {
            std::cerr << "error: no command specified after options\n";
            printUsage(argv[0]);
            return 1;
        }
        Runtime runtime;
        return runtime.run(args.container);
    }

    if (args.subcmd == "supervise") {
        if (args.container.cmd.empty()) {
            std::cerr << "error: no command specified after options\n";
            printUsage(argv[0]);
            return 1;
        }
        Supervisor supervisor;
        return supervisor.run(args.supervisor);
    }

    if (args.subcmd == "bench") {
        return runBenchmarks();
    }

    std::cerr << "error: unknown subcommand '" << args.subcmd << "'\n";
    printUsage(argv[0]);
    return 1;
}
