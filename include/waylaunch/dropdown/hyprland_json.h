#pragma once

#include <string>
#include <vector>

namespace waylaunch {

// Minimal JSON scanner for the two known-shape Hyprland IPC payloads
// (`j/clients`, `j/monitors`). Deliberately not a general parser: it extracts
// only the dozen fields the dropdown backend needs, so the repo avoids a JSON
// dependency for two fixed shapes. Unknown fields are skipped; missing fields
// keep their defaults; malformed input ends parsing and returns what was
// gathered so far.
struct HyprClient {
    std::string address;
    std::string klass;
    int pid = -1;
    int at_x = 0;
    int at_y = 0;
    int width = 0;
    int height = 0;
    int workspace_id = 0;
    std::string workspace_name;
    int monitor = 0;
    bool floating = false;
    bool mapped = false;
};

struct HyprMonitor {
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double scale = 1.0;
    bool focused = false;
    int active_workspace = 0;
    int reserved_top = 0;
    int reserved_bottom = 0;
};

std::vector<HyprClient> parse_hypr_clients(const std::string& json);
std::vector<HyprMonitor> parse_hypr_monitors(const std::string& json);

} // namespace waylaunch
