#pragma once

#include "waylaunch/dropdown/placement_backend.h"

#include <optional>
#include <string>
#include <vector>

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

// One `[[dropdown.slots]]` entry (§6). Percent overrides of -1 inherit the
// section default; an empty command falls back to the terminal probe.
struct DropdownSlot {
    std::string name;
    std::string command;
    int width_percent = -1;
    int height_percent = -1;
};

struct DropdownConfig {
    bool enabled = true;  // documented off switch for the whole host
    std::string terminal; // empty → $TERMINAL, then the probe list
    DropdownEdge edge = DropdownEdge::Top;
    int width_percent = 100; // of usable monitor width, clamped to [1, 100]
    int height_percent = 40; // of usable monitor height, clamped to [1, 100]
    // Persisted user resize (phase 4 state store). When set, its w/h replace
    // the computed size (clamped to the usable area); placement still follows
    // `edge`.
    std::optional<Geometry> size_override;
    // Focus-loss retract (phase 3).
    bool hide_on_focus_loss = true;
    int focus_grace_ms = 150; // ignore focus events this soon after a show
    bool respawn = true;
    std::string animation = "slide"; // compositor-side tuning hint (phase 6)
    // Owned tab strip (phase 5): thin layer surface above the terminal.
    bool tab_strip = true;
    // Named slots (phase 4). Empty → one implicit default slot.
    std::vector<DropdownSlot> slots;
};

Geometry compute_geometry(const MonitorInfo& monitor, const DropdownConfig& config);

// Phase 4 slot resolution (pure): merges the section defaults with the named
// slot's overrides. An unknown name yields the section defaults untouched, so
// ad-hoc `--dropdown scratch` keeps working without configuration.
struct ResolvedSlot {
    DropdownConfig config;
    std::string command;
};

ResolvedSlot resolve_dropdown_slot(const DropdownConfig& global, const std::string& slot);

// Gap-5 remainder: learn a manual resize instead of shipping a drag handle.
// The user reshapes the dropdown with the compositor's own bindings; the next
// hide compares what the window measures against what we last placed, and any
// real difference becomes the persisted override.
//
// `placed` is the terminal geometry we asked for and `observed` what it now
// measures. `strip_band` is the height the tab strip took off the top, added
// back so the stored size describes the whole dropdown rather than only the
// terminal beneath it. Differences under `epsilon` are compositor rounding
// and gap arithmetic, not intent. `observed_floating` must be true: a tiled
// (or otherwise externally reshaped) window is the compositor's doing, never
// evidence of a manual resize — learning it would permanently override the
// configured size with layout geometry. Returns nullopt when nothing was
// learned.
std::optional<Geometry> learn_resize(const Geometry& placed, const Geometry& observed,
                                     int strip_band, int epsilon, bool observed_floating);

} // namespace waylaunch
