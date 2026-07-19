#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <thread>
#include <mutex>
#include <signal.h>
#include <sys/types.h>

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
};

} // namespace waylaunch
