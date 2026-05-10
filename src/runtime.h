#pragma once

#include "cgroup.h"
#include "container.h"

#include <memory>
#include <sys/types.h>

class Runtime {
public:
    Runtime() = default;

    pid_t start(const ContainerConfig& cfg);
    int   wait(pid_t pid);
    int   waitNonBlocking(pid_t pid, int& exit_code);
    int   run(const ContainerConfig& cfg);

private:
    struct ChildArgs {
        const ContainerConfig* cfg;
        int                    pipe_read_fd;
    };

    static int  childEntry(void* arg);
    static void setupChild(const ContainerConfig& cfg, int pipe_read_fd);
    static bool mountProc();
    static bool setHostname(const std::string& hostname);
    static bool writeUidMap(pid_t pid, uid_t host_uid);
    static bool writeGidMap(pid_t pid, gid_t host_gid);
    static int  statusToExitCode(int wstatus);

    void finishRun(int exit_code);

    static constexpr size_t STACK_SIZE = 8 * 1024 * 1024;
    std::unique_ptr<Cgroup> active_cgroup_;
    pid_t                   active_pid_ = -1;
    char                    child_stack_[STACK_SIZE];
};
