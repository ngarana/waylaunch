#pragma once

#include <string>

namespace waylaunch {

// Phase 1 entry point, called from main.cpp after single-instance acquisition.
// Runs until SIGTERM/SIGINT. Returns the process exit code. An empty config
// path falls back to the default config location.
int dropdown_main(const std::string& slot, const std::string& config_path = "");

} // namespace waylaunch
