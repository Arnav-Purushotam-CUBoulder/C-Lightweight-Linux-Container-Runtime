#include "supervisor.h"

#include "logger.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

int Supervisor::signal_pipe_[2] = {-1, -1};

void Supervisor::signalHandler(int sig) {
    char byte = static_cast<char>(sig);
    (void)write(signal_pipe_[1], &byte, 1);
}

int Supervisor::run(const SupervisorConfig& cfg) {
    if (pipe(signal_pipe_) != 0) {
        LOG_ERROR("supervisor", "pipe() failed: ", strerror(errno));
        return -1;
    }
    fcntl(signal_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(signal_pipe_[1], F_SETFL, O_NONBLOCK);

    installSignalHandlers();
    logEvent("supervisor_start", cfg.container.id);

    int restart_count = 0;
    int final_code = -1;
    int stop_signal = 0;

    while (true) {
        auto t0 = std::chrono::steady_clock::now();
        pid_t child_pid = runtime_.start(cfg.container);
        if (child_pid < 0) break;

        logEvent("container_start",
                 "pid=" + std::to_string(child_pid) +
                 " run=" + std::to_string(restart_count));

        int exit_code = -1;
        while (true) {
            int state = runtime_.waitNonBlocking(child_pid, exit_code);
            if (state == 1) break;
            if (state < 0) {
                exit_code = -1;
                break;
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(signal_pipe_[0], &rfds);

            timeval timeout {};
            timeout.tv_usec = 200000;

            int ready = select(signal_pipe_[0] + 1, &rfds, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                if (ready < 0 && errno != EINTR) {
                    LOG_WARN("supervisor", "select() failed: ", strerror(errno));
                }
                continue;
            }

            char sig_byte = 0;
            while (read(signal_pipe_[0], &sig_byte, 1) == 1) {
                stop_signal = static_cast<unsigned char>(sig_byte);
                if (kill(child_pid, stop_signal) != 0 && errno != ESRCH) {
                    LOG_WARN("supervisor", "failed to forward signal ", stop_signal,
                             " to pid=", child_pid, ": ", strerror(errno));
                } else {
                    logEvent("signal_forward",
                             "pid=" + std::to_string(child_pid) +
                             " signal=" + std::to_string(stop_signal));
                }
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        last_stats_.wall_time_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        last_stats_.exit_code = exit_code;
        last_stats_.restart_count = restart_count;
        final_code = exit_code;

        logEvent("container_exit", "code=" + std::to_string(exit_code));

        if (stop_signal != 0) break;
        if (exit_code == 0 || restart_count >= cfg.max_restarts) break;

        ++restart_count;
        logEvent("container_restart",
                 "attempt=" + std::to_string(restart_count) +
                 " delay_ms=" + std::to_string(cfg.restart_delay_ms));
        usleep(cfg.restart_delay_ms * 1000);
    }

    if (stop_signal != 0) {
        logEvent("supervisor_stop", "signal=" + std::to_string(stop_signal));
    } else {
        logEvent("supervisor_stop", "final_code=" + std::to_string(final_code));
    }

    restoreSignalHandlers();
    close(signal_pipe_[0]);
    close(signal_pipe_[1]);
    return final_code;
}

void Supervisor::installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

void Supervisor::restoreSignalHandlers() {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

void Supervisor::logEvent(const char* event, const std::string& detail) const {
    auto now = std::chrono::system_clock::now();
    long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                       .count();
    printf("{\"ts\":%lld,\"event\":\"%s\",\"detail\":\"%s\"}\n",
           ts, event, detail.c_str());
    fflush(stdout);
}
