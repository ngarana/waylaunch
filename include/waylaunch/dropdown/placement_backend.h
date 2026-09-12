#pragma once

#include <optional>
#include <string>
#include <vector>

namespace waylaunch {

// Phase 1 of docs/DROPDOWN_IMPLEMENTATION.md: the dropdown core is pure logic
// behind this interface, so DropdownManager unit-tests without a compositor.
// Hyprland specifics land in HyprlandBackend (phase 2); a
// foreign-toplevel-only fallback can cover activate/close portably with
// supports_geometry() == false.
struct Geometry {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct MonitorInfo {
    std::string name;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    double scale = 1.0;
    int reserved_top = 0;
    int reserved_bottom = 0;
    int active_workspace = 0;
};

struct WindowInfo {
    std::string address;
    std::string app_id;
    std::string title;
    int pid = -1;
    Geometry geom;
    int workspace = 0;
    bool visible = false;
    bool active = false; // holds compositor focus right now
    bool floating = false;
};

class IPlacementBackend {
  public:
    virtual ~IPlacementBackend() = default;
    // Locates the slot window. `owner_pid` is the supervised terminal's pid
    // (-1 when unknown): a window owned by that process — or by one of its
    // descendants, since terminals re-exec and multiplex — wins over a bare
    // app-id match, so a same-class window the user spawned by hand is never
    // mistaken for the slot's own. The app-id match remains as the fallback
    // that lets a restarted daemon adopt the window it left behind.
    virtual std::optional<WindowInfo> find_window(const std::string& app_id,
                                                  int owner_pid = -1) = 0;
    // Every window belonging to this slot, in compositor order — the tab
    // strip's source of truth. Same ownership rule as find_window: with a
    // valid `owner_pid` only that process tree's windows are returned.
    virtual std::vector<WindowInfo> find_owned_windows(const std::string& app_id,
                                                       int owner_pid) = 0;
    // Lookup by compositor address (with or without the `0x` prefix), for
    // resolving event-stream payloads such as `activewindowv2>>ADDR`.
    virtual std::optional<WindowInfo> find_by_address(const std::string& address) = 0;
    virtual std::optional<MonitorInfo> focused_monitor() = 0;
    virtual bool show(const WindowInfo& window, const Geometry& geometry) = 0;
    virtual bool hide(const WindowInfo& window) = 0;
    virtual bool focus(const WindowInfo& window) = 0;
    // False means geometry placement is unavailable; the manager degrades to
    // show/hide-by-workspace only instead of failing.
    virtual bool supports_geometry() const = 0;
};

} // namespace waylaunch
