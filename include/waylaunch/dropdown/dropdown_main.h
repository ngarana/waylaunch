#pragma once

#include <string>

namespace waylaunch {

// Phase 1 entry point, called from main.cpp after single-instance acquisition.
// Runs until SIGTERM/SIGINT. Returns the process exit code.
int dropdown_main(const std::string& slot);

} // namespace waylaunch
