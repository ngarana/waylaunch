#pragma once

#include "waylaunch/switcher/toplevel_backend.h"
#include <string>
#include <vector>
#include <algorithm>

namespace waylaunch {

struct AppGroup {
    std::string app_id;
    std::string display_name;
    std::string icon_name;
    std::vector<ToplevelWindow> windows;
    uint64_t last_focused_time = 0;

    bool is_all_minimized() const {
        if (windows.empty()) return false;
        return std::all_of(windows.begin(), windows.end(), [](const auto& w) {
            return w.is_minimized;
        });
    }

    bool is_any_active() const {
        return std::any_of(windows.begin(), windows.end(), [](const auto& w) {
            return w.is_active;
        });
    }

    uintptr_t primary_handle() const {
        if (windows.empty()) return 0;
        // Prefer active window, then non-minimized, or first
        for (const auto& w : windows) {
            if (w.is_active) return w.handle_id;
        }
        for (const auto& w : windows) {
            if (!w.is_minimized) return w.handle_id;
        }
        return windows.front().handle_id;
    }
};

} // namespace waylaunch
