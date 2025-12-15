// ============================================================================
// supervisor.h — Process supervisor with lifecycle management
// ============================================================================
// CONCEPT: Process supervision
// ─────────────────────────────
// A supervisor wraps a container (or any child process) and provides:
//
//   Lifecycle management   — track start/stop/exit events with timestamps
//   Signal forwarding      — SIGTERM/SIGINT from the user propagate to the
//                            container PID 1, giving it a chance to shut down
//                            gracefully before the supervisor exits
//   Restart policy         — automatic restart on failure (configurable)
//   Structured logging     — every lifecycle event is a JSON log line so the
//                            run history is machine-parseable
//
// The supervisor sits between the user's shell and the Runtime:
//
//   user  ──SIGTERM──►  Supervisor  ──forward──►  Container (PID 1)
//   shell ◄──────────  Supervisor  ◄──waitpid──  Container exits
//
// Signal handling in a multi-process program requires care:
//   - We install a signal handler for SIGTERM/SIGINT/SIGCHLD.
//   - POSIX signal handlers can only safely call async-signal-safe functions.
//   - We use the self-pipe trick: the handler writes a byte to a pipe, and
//     the main loop polls the pipe with select() to react safely.
// ============================================================================
#pragma once

#include "container.h"
#include "runtime.h"
#include <atomic>
#include <sys/types.h>

class Supervisor {
public:
    // Run the container under supervision according to SupervisorConfig.
    // Returns the final exit code of the last container invocation.
    int run(const SupervisorConfig& cfg);

    // Returns the stats of the last completed run.
    RunStats lastStats() const { return last_stats_; }

private:
    // Run one container instance; record stats.
    // Returns exit code.
    int runOnce(const ContainerConfig& cfg, int run_index);

    // Install signal handlers (SIGTERM, SIGINT, SIGCHLD).
    void installSignalHandlers();

    // Restore default signal disposition.
    void restoreSignalHandlers();

    // Print a structured JSON lifecycle event.
    void logEvent(const char* event, const char* detail = "") const;

    // Self-pipe for async-signal-safe signal delivery.
    static int signal_pipe_[2];

    // Set by signal handler; read by main loop.
    static volatile sig_atomic_t got_signal_;
    static volatile sig_atomic_t last_signal_;

    static void signalHandler(int sig);

    RunStats  last_stats_{};
    Runtime   runtime_;
};
