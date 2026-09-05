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
};

} // namespace waylaunch
