// ============================================================================
// supervisor.cpp — Process supervisor implementation
// ============================================================================
#include "supervisor.h"
#include "logger.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ── Static members ────────────────────────────────────────────────────────────
int                     Supervisor::signal_pipe_[2] = {-1, -1};
volatile sig_atomic_t   Supervisor::got_signal_     = 0;
volatile sig_atomic_t   Supervisor::last_signal_    = 0;

// ── Signal handler ────────────────────────────────────────────────────────────
// This function is called asynchronously by the kernel.  Only async-signal-safe
// functions (write, _exit, etc.) are allowed here.  We use the self-pipe trick:
// write a byte to signal_pipe_[1]; the main loop reads from signal_pipe_[0].
void Supervisor::signalHandler(int sig) {
    got_signal_  = 1;
    last_signal_ = sig;
    char byte    = static_cast<char>(sig);
    // write() is async-signal-safe; ignore EINTR/EAGAIN here.
    (void)write(signal_pipe_[1], &byte, 1);
}

// ── Public ────────────────────────────────────────────────────────────────────

int Supervisor::run(const SupervisorConfig& cfg) {
    if (pipe(signal_pipe_) != 0) {
        LOG_ERROR("supervisor", "pipe() failed: ", strerror(errno));
        return -1;
    }
    // Make the write end non-blocking so the handler never stalls.
    fcntl(signal_pipe_[1], F_SETFL, O_NONBLOCK);

    installSignalHandlers();
    logEvent("supervisor_start", cfg.container.id.c_str());

    int restart_count = 0;
    int final_code    = 0;

    while (true) {
        logEvent("container_start",
                 ("run=" + std::to_string(restart_count)).c_str());

        auto t0 = std::chrono::steady_clock::now();
        final_code = runOnce(cfg.container, restart_count);
        auto t1 = std::chrono::steady_clock::now();

        last_stats_.wall_time_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        last_stats_.exit_code     = final_code;
        last_stats_.restart_count = restart_count;

        logEvent("container_exit",
                 ("code=" + std::to_string(final_code)).c_str());

        // Check if a termination signal arrived.
        if (got_signal_) {
            int sig = last_signal_;
            LOG_INFO("supervisor", "caught signal ", sig, " — stopping");
            logEvent("supervisor_stop", ("signal=" + std::to_string(sig)).c_str());
            break;
        }

        // Decide whether to restart based on policy.
        bool should_restart = false;
        switch (cfg.restart_policy) {
            case RestartPolicy::Always:
                should_restart = true;
                break;
            case RestartPolicy::OnFailure:
                should_restart = (final_code != 0);
                break;
            case RestartPolicy::Never:
                should_restart = false;
                break;
        }

        if (should_restart && restart_count < cfg.max_restarts) {
            ++restart_count;
            LOG_INFO("supervisor",
                     "restarting container (attempt ", restart_count,
                     "/", cfg.max_restarts, ")");
            // Brief delay before restart to avoid tight crash loops.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg.restart_delay_ms));
        } else {
            break;
        }
    }

    logEvent("supervisor_stop", ("final_code=" + std::to_string(final_code)).c_str());
    restoreSignalHandlers();
    close(signal_pipe_[0]);
    close(signal_pipe_[1]);

    return final_code;
}

// ── Private ───────────────────────────────────────────────────────────────────

int Supervisor::runOnce(const ContainerConfig& cfg, int run_index) {
    (void)run_index;
    return runtime_.run(cfg);
}

void Supervisor::installSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART: automatically restart interrupted syscalls where possible.
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);

    // Ignore SIGPIPE — can happen if log output is piped and reader closes.
    signal(SIGPIPE, SIG_IGN);
}

void Supervisor::restoreSignalHandlers() {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT,  SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

void Supervisor::logEvent(const char* event, const char* detail) const {
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                       .count();
    // Emit a structured JSON event on stdout so it's separate from the
    // container's own stderr output.
    printf("{\"ts\":%lld,\"event\":\"%s\",\"detail\":\"%s\"}\n",
           ts, event, detail);
    fflush(stdout);
}
