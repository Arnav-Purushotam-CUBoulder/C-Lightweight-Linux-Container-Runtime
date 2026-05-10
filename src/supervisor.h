#pragma once

#include "container.h"
#include "runtime.h"

#include <string>
#include <sys/types.h>

class Supervisor {
public:
    int run(const SupervisorConfig& cfg);
    RunStats lastStats() const { return last_stats_; }

private:
    void installSignalHandlers();
    void restoreSignalHandlers();
    void logEvent(const char* event, const std::string& detail = "") const;

    static void signalHandler(int sig);

    static int signal_pipe_[2];

    RunStats last_stats_{};
    Runtime  runtime_;
};
