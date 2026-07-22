#pragma once

#include "waylaunch/switcher/toplevel_backend.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

#ifdef HAS_FOREIGN_TOPLEVEL
#include "wlr-foreign-toplevel-management-client-protocol.h"
#endif

namespace waylaunch {

class WlrForeignToplevelBackend : public IToplevelBackend {
public:
    WlrForeignToplevelBackend() = default;
    ~WlrForeignToplevelBackend() override;

    bool init(wl_display* display, wl_registry* registry) override;
    void add_observer(IToplevelObserver* observer) override;
    void remove_observer(IToplevelObserver* observer) override;

    void activate(uintptr_t handle_id, wl_seat* seat) override;
    void close(uintptr_t handle_id) override;
    void set_minimized(uintptr_t handle_id, bool minimize) override;

    // Optional shell command run on activate (in addition to the protocol
    // request); the window is exported as $WL_APP_ID/$WL_CLASS/$WL_TITLE — a
    // compositor-agnostic hook for focus behaviour the protocol can't express
    // (e.g. workspace-following).
    void set_activate_command(std::string cmd) { activate_command_ = std::move(cmd); }

    const std::vector<ToplevelWindow>& windows() const override { return window_cache_; }

#ifdef HAS_FOREIGN_TOPLEVEL
    void bind_manager(zwlr_foreign_toplevel_manager_v1* manager);
    void handle_manager_toplevel(zwlr_foreign_toplevel_handle_v1* handle);
    void handle_toplevel_title(zwlr_foreign_toplevel_handle_v1* handle, const char* title);
    void handle_toplevel_app_id(zwlr_foreign_toplevel_handle_v1* handle, const char* app_id);
    void handle_toplevel_state(zwlr_foreign_toplevel_handle_v1* handle, wl_array* state);
    void handle_toplevel_closed(zwlr_foreign_toplevel_handle_v1* handle);
#endif

private:
    void sync_cache();
    void notify_created(const ToplevelWindow& win);
    void notify_updated(const ToplevelWindow& win);
    void notify_closed(uintptr_t handle_id);

    std::vector<IToplevelObserver*> observers_;
    std::vector<ToplevelWindow> window_cache_;
    std::string activate_command_;

#ifdef HAS_FOREIGN_TOPLEVEL
    zwlr_foreign_toplevel_manager_v1* manager_ = nullptr;
    std::unordered_map<uintptr_t, zwlr_foreign_toplevel_handle_v1*> handle_map_;
#endif
};

} // namespace waylaunch
