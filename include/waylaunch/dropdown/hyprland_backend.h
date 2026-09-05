#pragma once

#include "waylaunch/dropdown/placement_backend.h"

#include <string>

namespace waylaunch {

// Phase 2 Hyprland placement backend (docs/DROPDOWN_IMPLEMENTATION.md §5).
// Talks to the Hyprland IPC socket with targeted `/dispatch` commands per
// Spike 0 (§2): every action carries `window=<address>` resolved from
// `j/clients`, so placement never steals focus. All dispatcher strings live
// in hyprland_backend.cpp — nowhere else.
//
// Footgun guard (Spike 0 risk): never use `bring_to_top()` here. It takes no
// window argument and always raises the active window; targeted raise is
// `alter_zorder({ mode = "top", window })`, used by show().
class HyprlandBackend : public IPlacementBackend {
  public:
    explicit HyprlandBackend(std::string slot = "default") : slot_(std::move(slot)) {}

    std::optional<WindowInfo> find_window(const std::string& app_id) override;
    std::optional<WindowInfo> find_by_address(const std::string& address) override;
    std::optional<MonitorInfo> focused_monitor() override;
    bool show(const WindowInfo& window, const Geometry& geometry) override;
    bool hide(const WindowInfo& window) override;
    bool focus(const WindowInfo& window) override;
    bool supports_geometry() const override;

    const std::string& slot() const { return slot_; }
    std::string hidden_workspace() const { return "special:wl-drop-" + slot_; }

  private:
    std::string slot_;
};

} // namespace waylaunch
