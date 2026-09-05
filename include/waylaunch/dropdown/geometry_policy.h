#pragma once

#include "waylaunch/dropdown/placement_backend.h"

#include <optional>

namespace waylaunch {

// Phase 2 placement policy from docs/DROPDOWN_IMPLEMENTATION.md §5.
// Pure function of (monitor, config, optional override): no I/O, no
// compositor, unit-tested like power_layout. Hyprland reports monitor
// geometry and dispatches in logical pixels, so `scale` is carried on
// MonitorInfo for toolkits that need physical pixels but not applied here.
enum class DropdownEdge {
    Top,
    Bottom,
    Left,
    Right,
};

struct DropdownConfig {
    DropdownEdge edge = DropdownEdge::Top;
    int width_percent = 100; // of usable monitor width, clamped to [1, 100]
    int height_percent = 40; // of usable monitor height, clamped to [1, 100]
    // Persisted user resize (phase 4). When set, its w/h replace the computed
    // size (clamped to the usable area); placement still follows `edge`.
    std::optional<Geometry> size_override;
    // Focus-loss retract (phase 3). TOML wiring lands with §6; until then the
    // compiled defaults apply.
    bool hide_on_focus_loss = true;
    int focus_grace_ms = 150; // ignore focus events this soon after a show
};

Geometry compute_geometry(const MonitorInfo& monitor, const DropdownConfig& config);

} // namespace waylaunch
