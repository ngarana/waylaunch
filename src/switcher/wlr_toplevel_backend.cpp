#include "waylaunch/switcher/wlr_toplevel_backend.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

#ifdef HAS_FOREIGN_TOPLEVEL
#include <wayland-client.h>

namespace waylaunch {

constexpr uint32_t STATE_MAXIMIZED = 0;
constexpr uint32_t STATE_MINIMIZED = 1;
constexpr uint32_t STATE_ACTIVATED = 2;
constexpr uint32_t STATE_FULLSCREEN = 3;

extern "C" char** environ;

namespace {

// Run a user command template via /bin/sh with the selected window's properties
// exported as WL_APP_ID / WL_CLASS / WL_TITLE. Passing them through the
// environment (rather than string interpolation) keeps window titles from ever
// being reparsed as shell — no quoting hazards, no injection. This is the
// compositor-agnostic escape hatch for focus behaviour the protocol can't
// express, e.g. following a window to its workspace where `activate` doesn't.
void run_activate_command(const std::string& cmd, const ToplevelWindow& win) {
    if (cmd.empty()) return;
    // Build argv/envp in the parent so nothing between fork and exec allocates.
    std::string e_app = "WL_APP_ID=" + win.app_id;
    std::string e_cls = "WL_CLASS=" + win.app_id;   // alias; app_id == class here
    std::string e_ttl = "WL_TITLE=" + win.title;
    std::vector<char*> envp;
    for (char** e = environ; e && *e; ++e) envp.push_back(*e);
    envp.push_back(const_cast<char*>(e_app.c_str()));
    envp.push_back(const_cast<char*>(e_cls.c_str()));
    envp.push_back(const_cast<char*>(e_ttl.c_str()));
    envp.push_back(nullptr);
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                    const_cast<char*>(cmd.c_str()), nullptr};

    pid_t pid = fork();
    if (pid != 0) return;   // parent / fork failed
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }
    execve("/bin/sh", argv, envp.data());
    _exit(127);
}

void handle_toplevel_title_cb(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
    auto* self = static_cast<WlrForeignToplevelBackend*>(data);
    self->handle_toplevel_title(handle, title);
}

void handle_toplevel_app_id_cb(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* app_id) {
    auto* self = static_cast<WlrForeignToplevelBackend*>(data);
    self->handle_toplevel_app_id(handle, app_id);
}

void handle_toplevel_state_cb(void* data, zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
    auto* self = static_cast<WlrForeignToplevelBackend*>(data);
    self->handle_toplevel_state(handle, state);
}

void handle_toplevel_closed_cb(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* self = static_cast<WlrForeignToplevelBackend*>(data);
    self->handle_toplevel_closed(handle);
}

// Events we don't act on but MUST still handle: libwayland invokes the listener
// slot for every event the compositor sends, and a null slot would be called as
// a function pointer (crash). Their order in the struct below must match the
// protocol's event order (title, app_id, output_enter, output_leave, state,
// done, closed, parent) or opcodes desync — the exact bug this file had.
void handle_toplevel_output_enter_cb(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
void handle_toplevel_output_leave_cb(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {}
void handle_toplevel_done_cb(void*, zwlr_foreign_toplevel_handle_v1*) {}
void handle_toplevel_parent_cb(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) {}

const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .title = handle_toplevel_title_cb,
    .app_id = handle_toplevel_app_id_cb,
    .output_enter = handle_toplevel_output_enter_cb,
    .output_leave = handle_toplevel_output_leave_cb,
    .state = handle_toplevel_state_cb,
    .done = handle_toplevel_done_cb,
    .closed = handle_toplevel_closed_cb,
    .parent = handle_toplevel_parent_cb,
};

void handle_manager_toplevel_cb(void* data, zwlr_foreign_toplevel_manager_v1* /*manager*/, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* self = static_cast<WlrForeignToplevelBackend*>(data);
    self->handle_manager_toplevel(handle);
}

void handle_manager_finished_cb(void* /*data*/, zwlr_foreign_toplevel_manager_v1* /*manager*/) {}

const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = handle_manager_toplevel_cb,
    .finished = handle_manager_finished_cb,
};

} // namespace

WlrForeignToplevelBackend::~WlrForeignToplevelBackend() {
    if (manager_) {
        zwlr_foreign_toplevel_manager_v1_destroy(manager_);
        manager_ = nullptr;
    }
}

#ifdef HAS_FOREIGN_TOPLEVEL
void WlrForeignToplevelBackend::bind_manager(zwlr_foreign_toplevel_manager_v1* manager) {
    if (manager_ || !manager) return;
    manager_ = manager;
    zwlr_foreign_toplevel_manager_v1_add_listener(manager_, &manager_listener, this);
}
#endif

bool WlrForeignToplevelBackend::init(wl_display* /*display*/, wl_registry* registry) {
    if (!registry) return false;
    return true;
}

void WlrForeignToplevelBackend::add_observer(IToplevelObserver* observer) {
    if (observer) {
        observers_.push_back(observer);
    }
}

void WlrForeignToplevelBackend::remove_observer(IToplevelObserver* observer) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());
}

void WlrForeignToplevelBackend::activate(uintptr_t handle_id, wl_seat* seat) {
    if (!seat) return;   // activate requires the seat that owns the input focus
    auto it = handle_map_.find(handle_id);
    if (it == handle_map_.end()) return;
    zwlr_foreign_toplevel_handle_v1_activate(it->second, seat);
    // Optional user hook: run a command to complete focus behaviour the protocol
    // can't express (e.g. following the window to its workspace on compositors
    // where `activate` alone doesn't).
    if (!activate_command_.empty()) {
        for (const auto& w : window_cache_) {
            if (w.handle_id == handle_id) { run_activate_command(activate_command_, w); break; }
        }
    }
}

void WlrForeignToplevelBackend::close(uintptr_t handle_id) {
    auto it = handle_map_.find(handle_id);
    if (it != handle_map_.end()) {
        zwlr_foreign_toplevel_handle_v1_close(it->second);
    }
}

void WlrForeignToplevelBackend::set_minimized(uintptr_t handle_id, bool minimize) {
    auto it = handle_map_.find(handle_id);
    if (it == handle_map_.end()) return;
    if (minimize) zwlr_foreign_toplevel_handle_v1_set_minimized(it->second);
    else          zwlr_foreign_toplevel_handle_v1_unset_minimized(it->second);
}

void WlrForeignToplevelBackend::handle_manager_toplevel(zwlr_foreign_toplevel_handle_v1* handle) {
    if (!handle) return;
    uintptr_t id = reinterpret_cast<uintptr_t>(handle);
    handle_map_[id] = handle;

    ToplevelWindow win;
    win.handle_id = id;
    window_cache_.push_back(win);

    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, this);
    notify_created(win);
}

void WlrForeignToplevelBackend::handle_toplevel_title(zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
    uintptr_t id = reinterpret_cast<uintptr_t>(handle);
    for (auto& win : window_cache_) {
        if (win.handle_id == id) {
            win.title = title ? title : "";
            notify_updated(win);
            break;
        }
    }
}

void WlrForeignToplevelBackend::handle_toplevel_app_id(zwlr_foreign_toplevel_handle_v1* handle, const char* app_id) {
    uintptr_t id = reinterpret_cast<uintptr_t>(handle);
    for (auto& win : window_cache_) {
        if (win.handle_id == id) {
            win.app_id = app_id ? app_id : "";
            notify_updated(win);
            break;
        }
    }
}

void WlrForeignToplevelBackend::handle_toplevel_state(zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
    uintptr_t id = reinterpret_cast<uintptr_t>(handle);
    for (auto& win : window_cache_) {
        if (win.handle_id == id) {
            win.is_active = false;
            win.is_minimized = false;
            win.is_maximized = false;
            win.is_fullscreen = false;

            if (state && state->data) {
                uint32_t* entries = static_cast<uint32_t*>(state->data);
                size_t count = state->size / sizeof(uint32_t);
                for (size_t i = 0; i < count; ++i) {
                    switch (entries[i]) {
                        case STATE_ACTIVATED:
                            win.is_active = true;
                            break;
                        case STATE_MINIMIZED:
                            win.is_minimized = true;
                            break;
                        case STATE_MAXIMIZED:
                            win.is_maximized = true;
                            break;
                        case STATE_FULLSCREEN:
                            win.is_fullscreen = true;
                            break;
                    }
                }
            }
            notify_updated(win);
            break;
        }
    }
}

void WlrForeignToplevelBackend::handle_toplevel_closed(zwlr_foreign_toplevel_handle_v1* handle) {
    uintptr_t id = reinterpret_cast<uintptr_t>(handle);
    handle_map_.erase(id);

    window_cache_.erase(
        std::remove_if(window_cache_.begin(), window_cache_.end(), [id](const ToplevelWindow& w) {
            return w.handle_id == id;
        }),
        window_cache_.end()
    );

    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    notify_closed(id);
}

void WlrForeignToplevelBackend::notify_created(const ToplevelWindow& win) {
    for (auto* obs : observers_) obs->on_window_created(win);
}

void WlrForeignToplevelBackend::notify_updated(const ToplevelWindow& win) {
    for (auto* obs : observers_) obs->on_window_updated(win);
}

void WlrForeignToplevelBackend::notify_closed(uintptr_t handle_id) {
    for (auto* obs : observers_) obs->on_window_closed(handle_id);
}

} // namespace waylaunch
#else
namespace waylaunch {
WlrForeignToplevelBackend::~WlrForeignToplevelBackend() = default;
bool WlrForeignToplevelBackend::init(wl_display*, wl_registry*) { return false; }
void WlrForeignToplevelBackend::add_observer(IToplevelObserver*) {}
void WlrForeignToplevelBackend::remove_observer(IToplevelObserver*) {}
void WlrForeignToplevelBackend::activate(uintptr_t, wl_seat*) {}
void WlrForeignToplevelBackend::close(uintptr_t) {}
void WlrForeignToplevelBackend::set_minimized(uintptr_t, bool) {}
} // namespace waylaunch
#endif
