#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace waylaunch {

struct ProcessResult {
    int exit_code = -1;
    std::string stdout;
    std::string stderr;
};

class Subprocess {
  public:
    static ProcessResult run(const std::vector<std::string>& argv,
                             const std::string& stdin_data = "");
    static bool command_exists(const std::string& command);
    // Launch fully detached from waylaunch (double-fork + setsid) with an argv
    // vector, so arguments are never re-parsed by a shell. Fire-and-forget: the
    // grandchild is reparented to init.
    static void spawn_detached(const std::vector<std::string>& argv);
    // Launch a supervised child (single fork + setsid) and return its pid for
    // waitpid/SIGCHLD reaping. Unlike spawn_detached the child stays ours, so
    // the dropdown session supervisor can respawn it. Returns -1 on failure.
    static pid_t spawn_tracked(const std::vector<std::string>& argv);
};

} // namespace waylaunch
