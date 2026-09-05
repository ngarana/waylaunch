#include "waylaunch/dropdown/geometry_policy.h"

#include <algorithm>

namespace waylaunch {
namespace {

// Percent of `total`, clamped so a misconfigured >100% cannot push the
// terminal off-screen. long long keeps width * percent overflow-clean.
int percent_of(int total, int percent) {
    int clamped = std::clamp(percent, 1, 100);
    long long scaled = static_cast<long long>(total) * static_cast<long long>(clamped);
    return std::max(1, static_cast<int>(scaled / 100));
}

} // namespace

Geometry compute_geometry(const MonitorInfo& monitor, const DropdownConfig& config) {
    int usable_x = monitor.x;
    int usable_y = monitor.y + monitor.reserved_top;
    int usable_w = std::max(1, monitor.w);
    int usable_h = std::max(1, monitor.h - monitor.reserved_top - monitor.reserved_bottom);

    int w = percent_of(usable_w, config.width_percent);
    int h = percent_of(usable_h, config.height_percent);
    if (config.size_override.has_value()) {
        w = std::clamp(config.size_override->w, 1, usable_w);
        h = std::clamp(config.size_override->h, 1, usable_h);
    }

    Geometry geom{.x = usable_x, .y = usable_y, .w = w, .h = h};
    switch (config.edge) {
        case DropdownEdge::Top: break;
        case DropdownEdge::Bottom: geom.y = usable_y + usable_h - h; break;
        case DropdownEdge::Left: break;
        case DropdownEdge::Right: geom.x = usable_x + usable_w - w; break;
    }
    return geom;
}

ResolvedSlot resolve_dropdown_slot(const DropdownConfig& global, const std::string& slot) {
    ResolvedSlot resolved{.config = global, .command = ""};
    for (const DropdownSlot& entry : global.slots) {
        if (entry.name != slot) continue;
        if (!entry.command.empty()) resolved.command = entry.command;
        if (entry.width_percent >= 0) resolved.config.width_percent = entry.width_percent;
        if (entry.height_percent >= 0) resolved.config.height_percent = entry.height_percent;
        break;
    }
    return resolved;
}

} // namespace waylaunch
