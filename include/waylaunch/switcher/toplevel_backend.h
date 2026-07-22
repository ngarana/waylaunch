#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

struct wl_display;
struct wl_registry;
struct wl_seat;

namespace waylaunch {

struct ToplevelWindow {
    uintptr_t handle_id = 0;
    std::string app_id;
    std::string title;
    std::string icon_name;
    bool is_active = false;
    bool is_minimized = false;
    bool is_maximized = false;
    bool is_fullscreen = false;
};

class IToplevelObserver {
public:
    virtual ~IToplevelObserver() = default;
    virtual void on_window_created(const ToplevelWindow& window) = 0;
    virtual void on_window_updated(const ToplevelWindow& window) = 0;
    virtual void on_window_closed(uintptr_t handle_id) = 0;
};

class IToplevelBackend {
public:
    virtual ~IToplevelBackend() = default;
    virtual bool init(wl_display* display, wl_registry* registry) = 0;
    virtual void add_observer(IToplevelObserver* observer) = 0;
    virtual void remove_observer(IToplevelObserver* observer) = 0;
    virtual void activate(uintptr_t handle_id, wl_seat* seat) = 0;
    virtual void close(uintptr_t handle_id) = 0;
    virtual void set_minimized(uintptr_t handle_id, bool minimize) = 0;
    virtual const std::vector<ToplevelWindow>& windows() const = 0;
};

} // namespace waylaunch
