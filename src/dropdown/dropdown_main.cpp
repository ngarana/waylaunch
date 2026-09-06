#include "waylaunch/dropdown/dropdown_main.h"

#include "waylaunch/config.h"
#include "waylaunch/dropdown/dropdown_manager.h"
#include "waylaunch/dropdown/dropdown_state.h"
#include "waylaunch/dropdown/focus_guard.h"
#include "waylaunch/dropdown/geometry_policy.h"
#include "waylaunch/dropdown/hyprland_backend.h"
#include "waylaunch/dropdown/hyprland_events.h"
#include "waylaunch/dropdown/session_supervisor.h"
#include "waylaunch/dropdown/tab_strip.h"
#include "waylaunch/renderer.h"
#include "waylaunch/subprocess.h"
#include "waylaunch/switcher/toplevel_backend.h"
#include "waylaunch/switcher/wlr_toplevel_backend.h"
#include "waylaunch/wayland_core.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

namespace waylaunch {
namespace {

void arm_timer(int timer_fd, std::chrono::milliseconds delay) {
    itimerspec spec{};
    spec.it_value.tv_sec = delay.count() / 1000;
    spec.it_value.tv_nsec = (delay.count() % 1000) * 1000000;
    timerfd_settime(timer_fd, 0, &spec, nullptr);
}

void disarm_timer(int timer_fd) {
    itimerspec spec{};
    timerfd_settime(timer_fd, 0, &spec, nullptr);
}

// How long to wait for a freshly spawned terminal to appear in j/clients:
// 25 attempts × 200ms ≈ 5s, then assume hidden and let later toggles resync.
constexpr int kAppearAttempts = 25;
constexpr std::chrono::milliseconds kAppearRetry{200};

constexpr uint32_t kBtnLeft = 0x110;

void sync_done(void* data, wl_callback*, uint32_t) { *static_cast<bool*>(data) = true; }

// wl_display_roundtrip with a deadline. A raw roundtrip blocks forever when
// the compositor stalls (or the display is already poisoned), which would
// wedge the daemon deaf: all signals are blocked for signalfd, so only the
// poll loop services them. On timeout/error returns false and the caller
// degrades instead of hanging.
bool display_roundtrip_bounded(wl_display* dpy, int timeout_ms) {
    static const wl_callback_listener kSyncListener = {.done = sync_done};
    bool done = false;
    wl_callback* callback = wl_display_sync(dpy);
    if (callback == nullptr) return false;
    wl_callback_add_listener(callback, &kSyncListener, &done);
    wl_display_flush(dpy);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    // Never wl_display_dispatch here: it performs a blocking read, which
    // parks forever when no reply comes (observed live as a deaf daemon via
    // a core backtrace). Only the prepare/read/pending trio, each bounded.
    bool prepared = false;
    while (!done) {
        if (wl_display_get_error(dpy) != 0) break;
        if (!prepared) {
            if (wl_display_prepare_read(dpy) != 0) {
                if (wl_display_dispatch_pending(dpy) < 0) break;
                continue;
            }
            prepared = true;
        }
        int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             deadline - std::chrono::steady_clock::now())
                                             .count());
        if (remaining <= 0) break;
        pollfd pfd{.fd = wl_display_get_fd(dpy), .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break; // deadline elapsed without the done event
        if (wl_display_read_events(dpy) < 0) break;
        prepared = false;
        if (wl_display_dispatch_pending(dpy) < 0) break;
    }
    if (prepared) wl_display_cancel_read(dpy);
    wl_callback_destroy(callback);
    return done;
}

// Pokes the strip render flag whenever the toplevel set changes, so tabs
// track window open/close without polling. Owned by dropdown_main's frame.
class StripObserver : public IToplevelObserver {
  public:
    explicit StripObserver(bool& dirty) : dirty_(dirty) {}
    void on_window_created(const ToplevelWindow&) override { dirty_ = true; }
    void on_window_updated(const ToplevelWindow&) override { dirty_ = true; }
    void on_window_closed(uintptr_t) override { dirty_ = true; }

  private:
    bool& dirty_;
};

} // namespace

int dropdown_main(const std::string& slot, const std::string& config_path) {
    SessionSupervisor supervisor(slot);
    DropdownManager manager;
    HyprlandBackend backend(slot);

    // Section defaults plus this slot's overrides (§6). Unknown slot names
    // yield the section untouched, so ad-hoc `--dropdown scratch` works.
    waylaunch::Config repo_config;
    const std::string path =
        config_path.empty() ? waylaunch::Config::default_config_path() : config_path;
    if (!repo_config.load(path)) {
        std::cerr << "Warning: Could not load config, using dropdown defaults.\n";
    }
    if (!repo_config.get().dropdown.enabled) {
        std::cout << "waylaunch: dropdown overlay disabled ([dropdown].enabled is false)\n";
        return 0;
    }
    ResolvedSlot resolved = resolve_dropdown_slot(repo_config.get().dropdown, slot);
    DropdownConfig config = resolved.config;
    DropdownStateStore state_store;
    auto states = state_store.load();
    if (auto it = states.find(slot); it != states.end()) {
        config.size_override = Geometry{.x = 0, .y = 0, .w = it->second.w, .h = it->second.h};
    }
    supervisor.set_respawn_enabled(config.respawn);

    FocusGuard guard;
    guard.configure(config.hide_on_focus_loss, config.focus_grace_ms);
    HyprlandEventStream events;
    bool backend_usable = backend.supports_geometry();
    // Last address the slot window was seen at. closewindow arrives after the
    // window is already gone from j/clients, so identity must come from here
    // rather than a fresh lookup.
    std::string slot_address;

    // Phase 5 tab strip: first Wayland in this daemon. Lazily initialized on
    // first show so lifecycle-only (and non-Wayland) environments never pay
    // for it; dormant (unmapped) while hidden.
    WaylandCore wayland;
    Renderer strip_renderer;
    TabStrip tab_strip;
#ifdef HAS_FOREIGN_TOPLEVEL
    std::unique_ptr<WlrForeignToplevelBackend> toplevel;
#endif
    bool strip_ready = false;
    bool strip_needs_render = false;
    bool wayland_dead = false;    // protocol error: strip off for the session
    int strip_rendered_width = 0; // last painted width; hit-testing coordinate space
    StripObserver strip_observer(strip_needs_render);

    // Block the signals we multiplex through signalfd so they never run as
    // async handlers mid-fork.
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) return 1;

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) return 1;
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
        close(signal_fd);
        return 1;
    }

    // What the single timerfd is currently counting down for.
    enum class TimerPurpose {
        None,
        Respawn,    // backoff elapsed after a death: fork again
        AppearRetry // spawned window not yet in j/clients: look again
    };
    TimerPurpose timer_purpose = TimerPurpose::None;
    int appear_attempts = 0;

    auto arm = [&](TimerPurpose purpose, std::chrono::milliseconds delay) {
        timer_purpose = purpose;
        arm_timer(timer_fd, delay);
    };

    // This slot's toplevels, for the strip. App-id comes from the
    // foreign-toplevel manager; `kitty --class` sets it to the slot app-id.
    auto collect_tabs = [&]() {
        std::vector<TabStrip::Tab> tabs;
#ifdef HAS_FOREIGN_TOPLEVEL
        if (toplevel) {
            for (const ToplevelWindow& window : toplevel->windows()) {
                if (window.app_id == supervisor.app_id()) {
                    tabs.push_back({.handle_id = window.handle_id,
                                    .title = window.title,
                                    .is_active = window.is_active});
                }
            }
        }
#endif
        tab_strip.update(std::move(tabs));
    };

    auto render_strip = [&]() {
        if (!strip_ready || !wayland.is_configured()) return;
        if (manager.current_state() != DropdownState::Visible) return;
        collect_tabs();
        Buffer* buf = wayland.acquire_buffer();
        if (buf == nullptr) return;
        TabStrip::Colors colors{
            .background = Color::from_hex(repo_config.get().theme.colors.background),
            .foreground = Color::from_hex(repo_config.get().theme.colors.foreground),
            .accent = Color::from_hex(repo_config.get().theme.colors.accent),
        };
        RenderFontConfig font;
        font.family = repo_config.get().theme.result_font.family;
        font.size = repo_config.get().theme.result_font.size;
        strip_renderer.begin(buf->data, buf->stride, buf->width, buf->height);
        tab_strip.render(strip_renderer, buf->width, colors, font);
        strip_renderer.end();
        strip_rendered_width = buf->width;
        wayland.submit_buffer(buf, 0, 0);
    };

    auto ensure_strip = [&]() -> bool {
        if (!config.tab_strip || !backend_usable || wayland_dead) return false;
#ifdef HAS_FOREIGN_TOPLEVEL
        if (strip_ready) return true;
        toplevel = std::make_unique<WlrForeignToplevelBackend>();
        // Listener BEFORE init(): the manager global binds during init's
        // roundtrip and immediately emits existing toplevels; registering
        // late would miss every already-open window (switcher pattern).
        // Namespace is fixed at surface creation, so seed it here too —
        // remap_surface() only re-applies anchor/size/interactivity/zone.
        // NOTE: this pre-seed must stay protocol-legal on its own (Hyprland
        // kills surfaces committed with a zero size on a partial anchor
        // set), so it keeps all four anchors with a zero span.
        wayland.set_want_backdrop(false);
        {
            LayerSurfaceConfig initial;
            initial.keyboard = LayerKeyboardMode::None;
            initial.exclusive_zone = 0;
            initial.layer_namespace = "waylaunch-dropdown-tabs";
            wayland.set_layer_surface_config(initial);
        }
        wayland.set_foreign_toplevel_listener([&](zwlr_foreign_toplevel_manager_v1* mgr) {
            if (toplevel) toplevel->bind_manager(mgr);
        });
        if (!wayland.init()) {
            toplevel.reset();
            return false;
        }
        if (wayland.foreign_toplevel_manager() != nullptr) {
            toplevel->bind_manager(wayland.foreign_toplevel_manager());
        }
        toplevel->add_observer(&strip_observer);
        wayland.set_mouse_handler([&](double x, double y, uint32_t button, bool pressed) {
            if (!pressed || button != kBtnLeft) return;
            if (manager.current_state() != DropdownState::Visible || !strip_ready) return;
            uintptr_t target =
                tab_strip.hit_test(static_cast<int>(x), static_cast<int>(y), strip_rendered_width);
            if (target != 0 && toplevel && wayland.seat() != nullptr) {
                toplevel->activate(target, wayland.seat());
            }
        });
        wayland.set_redraw_handler([&]() { strip_needs_render = true; });
        wayland.set_close_handler([&]() { strip_ready = false; });
        strip_ready = true;
        return true;
#else
        return false;
#endif
    };

    // Map the strip flush above the placed terminal and paint it.
    // Margins are relative to the layer working area (output origin pushed
    // past other exclusive zones like the bar), NOT to the output origin —
    // verified live: a margin of reserved_top landed the strip a full bar
    // height too low. working_top/left anchor the computation.
    auto show_strip = [&](const Geometry& rect, int working_top, int working_left) {
        if (!ensure_strip()) return;
        LayerSurfaceConfig layer;
        layer.anchors =
            static_cast<uint32_t>(LayerAnchor::Top) | static_cast<uint32_t>(LayerAnchor::Left);
        layer.width = rect.w;
        layer.height = rect.h;
        layer.margin_top = rect.y - working_top;
        layer.margin_left = rect.x - working_left;
        layer.keyboard = LayerKeyboardMode::None;
        layer.exclusive_zone = 0; // terminal is placed manually; don't shift tiling
        layer.layer_namespace = "waylaunch-dropdown-tabs";
        wayland.set_layer_surface_config(layer);
        wayland.remap_surface();
        // Collect the configure synchronously (the switcher's map-NOW pattern):
        // without this the first show would wait for the next event. Bounded:
        // an unanswered sync must degrade to a later paint, never wedge us.
        wl_display* dpy = wayland.display();
        if (dpy != nullptr) display_roundtrip_bounded(dpy, 1000);
        strip_needs_render = true;
        render_strip();
    };

    auto spawn = [&]() {
        // Slot command wins; otherwise [dropdown].terminal/probe, so the
        // window always carries the slot app-id for find_window.
        std::vector<std::string> argv =
            resolved.command.empty()
                ? SessionSupervisor::build_argv(config.terminal, supervisor.app_id())
                : SessionSupervisor::build_slot_argv(resolved.command, supervisor.app_id());
        pid_t pid = Subprocess::spawn_tracked(argv);
        auto now = std::chrono::steady_clock::now();
        if (pid > 0) {
            supervisor.note_spawned(pid, now);
            guard.set_slot(pid, ""); // address resolves once mapped
            // The window takes ~100ms to appear; poll j/clients until it
            // does, then park it on the hidden workspace (initial Hidden).
            appear_attempts = 0;
            arm(TimerPurpose::AppearRetry, kAppearRetry);
        } else {
            // Fork failed: retry with backoff rather than hot-looping.
            auto delay = supervisor.note_exited(now);
            if (delay.has_value()) arm(TimerPurpose::Respawn, *delay);
        }
    };

    // Place the slot window per the recomputed-on-every-show policy (§5.2):
    // geometry derives from the focused monitor, so hotplugged outputs and
    // resolution changes need no restart. With the tab strip on, the terminal
    // yields its top kHeight px; the strip takes that band. Returns the placed
    // window.
    auto place_visible = [&]() -> std::optional<WindowInfo> {
        auto window = backend.find_window(supervisor.app_id());
        auto monitor = backend.focused_monitor();
        if (!window.has_value() || !monitor.has_value()) return std::nullopt;
        Geometry base = compute_geometry(*monitor, config);
        Geometry term = base;
        bool strip = config.tab_strip && backend_usable && ensure_strip();
        if (strip) {
            term.y += TabStrip::kHeight;
            term.h = std::max(1, term.h - TabStrip::kHeight);
        }
        if (!backend.show(*window, term)) return std::nullopt;
        if (strip) {
            show_strip(Geometry{.x = base.x, .y = base.y, .w = term.w, .h = TabStrip::kHeight},
                       monitor->y + monitor->reserved_top, monitor->x);
        }
        return window;
    };

    auto park_hidden = [&]() -> std::optional<WindowInfo> {
        auto window = backend.find_window(supervisor.app_id());
        if (!window.has_value()) return std::nullopt;
        if (!backend.hide(*window)) return std::nullopt;
        return window;
    };

    // Reconcile manager state with compositor truth. Used on toggle paths;
    // returns false when there is no window to act on.
    auto sync_presence = [&]() {
        if (backend_usable && backend.find_window(supervisor.app_id()).has_value()) return true;
        if (!supervisor.has_child() && manager.current_state() != DropdownState::Absent) {
            manager.process_event(DropdownEvent::WindowClosed);
        }
        return false;
    };

    auto hide_strip = [&]() {
        if (strip_ready) wayland.unmap_surface();
    };

    auto on_toggle = [&]() {
        auto before = manager.current_state();
        manager.process_event(DropdownEvent::Toggle);
        if (!backend_usable) return; // degraded: lifecycle only, no placement
        auto now = std::chrono::steady_clock::now();
        if (before == DropdownState::Absent && manager.current_state() == DropdownState::Spawning) {
            spawn(); // the `exit` → press-again → fresh-terminal case
        } else if (manager.current_state() == DropdownState::Visible) {
            auto shown = sync_presence() ? place_visible() : std::optional<WindowInfo>{};
            if (!shown.has_value()) {
                manager.process_event(DropdownEvent::WindowHidden); // revert
            } else {
                slot_address = shown->address;
                guard.set_slot(supervisor.child_pid(), slot_address);
                guard.note_shown(now);
            }
        } else if (manager.current_state() == DropdownState::Hidden) {
            if (sync_presence()) {
                if (park_hidden().has_value()) {
                    hide_strip();
                } else {
                    manager.process_event(DropdownEvent::WindowShown); // revert
                }
            }
        }
    };

    // Focus-loss retract (§5.3): another window took focus while visible.
    auto on_focus_event = [&](const std::string& address) {
        if (manager.current_state() != DropdownState::Visible || !backend_usable) return;
        auto focused = backend.find_by_address(address);
        int pid = focused.has_value() ? focused->pid : -1;
        if (guard.should_retract(std::chrono::steady_clock::now(), address, pid)) {
            if (park_hidden().has_value()) {
                manager.process_event(DropdownEvent::WindowHidden);
                hide_strip();
            }
            // On failure stay Visible; the next focus event retries.
        }
    };

    // Death detection: the slot window closed behind our back (the process
    // may still be alive, so kill it — its SIGCHLD then reaps harmlessly
    // against the cleared pid and the slot reboots on the next toggle).
    auto on_close_event = [&](const std::string& address) {
        if (!backend_usable || slot_address.empty()) return;
        if (normalize_address(address) != normalize_address(slot_address)) return;
        slot_address.clear();
        manager.process_event(DropdownEvent::WindowClosed);
        hide_strip();
        if (auto pid = supervisor.take_child(); pid.has_value()) { kill(*pid, SIGTERM); }
    };

    // First press appears (gap-3 fix): boot the session on start.
    manager.process_event(DropdownEvent::Toggle);
    spawn();

    bool running = true;
    while (running) {
        events.ensure_connected(std::chrono::steady_clock::now());
        // Wayland dispatch around poll (launcher pattern): prepare before
        // blocking, read-or-cancel after. Only while the strip is up.
        wl_display* wl_dpy = (!wayland_dead && strip_ready) ? wayland.display() : nullptr;
        if (wl_dpy != nullptr && wl_display_get_error(wl_dpy) != 0) {
            // A protocol error poisons the connection: prepare_read would
            // spin forever and signals would never be serviced (all blocked
            // for signalfd). Shed the strip; lifecycle/placement continue.
            std::fprintf(stderr, "waylaunch-dropdown: wayland protocol error, tab strip off\n");
            wayland_dead = true;
            strip_ready = false;
            wl_dpy = nullptr;
        }
        if (wl_dpy != nullptr) {
            // Bounded: a never-draining queue must still reach poll() so
            // signals stay serviced.
            for (int i = 0; i < 100 && wl_display_prepare_read(wl_dpy) != 0; ++i) {
                wl_display_dispatch_pending(wl_dpy);
            }
            wl_display_flush(wl_dpy);
        }
        pollfd fds[4]{};
        fds[0].fd = signal_fd;
        fds[0].events = POLLIN;
        fds[1].fd = timer_fd;
        fds[1].events = POLLIN;
        fds[2].fd = events.poll_fd(); // -1 while disconnected: ignored by poll
        fds[2].events = POLLIN;
        fds[3].fd = (wl_dpy != nullptr) ? wl_display_get_fd(wl_dpy) : -1;
        fds[3].events = POLLIN;
        int n = poll(fds, 4, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            // Both fds are nonblocking, so these drain loops terminate with
            // EAGAIN once pending events are consumed. Never drop the
            // NONBLOCK flags: a blocking read here would hang the daemon
            // after the last pending signal (seen live via wchan).
            signalfd_siginfo info{};
            while (read(signal_fd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
                if (info.ssi_signo == static_cast<uint32_t>(SIGUSR1)) {
                    on_toggle();
                } else if (info.ssi_signo == static_cast<uint32_t>(SIGCHLD)) {
                    int status = 0;
                    pid_t waited = 0;
                    while ((waited = waitpid(-1, &status, WNOHANG)) > 0) {
                        if (waited == supervisor.child_pid()) {
                            auto now = std::chrono::steady_clock::now();
                            manager.process_event(DropdownEvent::ChildExited);
                            auto delay = supervisor.note_exited(now);
                            if (delay.has_value()) arm(TimerPurpose::Respawn, *delay);
                        }
                    }
                } else {
                    running = false;
                }
            }
        }
        if ((fds[1].revents & POLLIN) != 0) {
            uint64_t expirations = 0;
            while (read(timer_fd, &expirations, sizeof(expirations)) ==
                   static_cast<ssize_t>(sizeof(expirations))) {}
            // Consumed: disarm before branching; each path re-arms as needed.
            disarm_timer(timer_fd);
            if (timer_purpose == TimerPurpose::Respawn) {
                timer_purpose = TimerPurpose::None;
                // Backoff elapsed after a death: re-enter Spawning and fork.
                if (manager.current_state() == DropdownState::Absent) {
                    manager.process_event(DropdownEvent::Toggle);
                }
                if (manager.current_state() == DropdownState::Spawning) spawn();
            } else if (timer_purpose == TimerPurpose::AppearRetry) {
                // Fresh child not yet in j/clients: hide it onto the slot's
                // hidden workspace once it appears (initial Hidden).
                bool placed = false;
                if (backend_usable) {
                    if (auto parked = park_hidden(); parked.has_value()) {
                        placed = true;
                        slot_address = parked->address;
                        guard.set_slot(supervisor.child_pid(), slot_address);
                        manager.process_event(DropdownEvent::WindowHidden);
                    }
                } else {
                    placed = true; // no compositor to observe; assume hidden
                    manager.process_event(DropdownEvent::WindowHidden);
                }
                if (!placed && manager.current_state() == DropdownState::Spawning) {
                    if (++appear_attempts < kAppearAttempts) {
                        arm(TimerPurpose::AppearRetry, kAppearRetry);
                    } else {
                        timer_purpose = TimerPurpose::None;
                        manager.process_event(DropdownEvent::WindowHidden);
                    }
                } else {
                    timer_purpose = TimerPurpose::None;
                }
            }
        }
        if (fds[2].revents & POLLIN) {
            for (const HyprEvent& event : events.read_available()) {
                if (event.name == "activewindowv2") {
                    on_focus_event(event.payload);
                } else if (event.name == "closewindow") {
                    on_close_event(event.payload);
                }
                // focusedmon: monitor following already happens through the
                // focused-monitor read on every show; while visible we
                // deliberately do not chase, so user drags are never fought.
            }
        }
        if (wl_dpy != nullptr) {
            if ((fds[3].revents & POLLIN) != 0) {
                if (wl_display_read_events(wl_dpy) < 0) running = false;
            } else {
                wl_display_cancel_read(wl_dpy);
            }
            wl_display_dispatch_pending(wl_dpy);
            if (strip_needs_render) {
                strip_needs_render = false;
                render_strip();
            }
        }
    }

    if (supervisor.has_child()) {
        kill(supervisor.child_pid(), SIGTERM);
        // Bounded reap: give the terminal a grace period, then escalate.
        // A blocking waitpid here would hang shutdown on a stubborn child.
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 50; ++i) {
            pid_t waited = waitpid(supervisor.child_pid(), &status, WNOHANG);
            if (waited != 0) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            kill(supervisor.child_pid(), SIGKILL);
            waitpid(supervisor.child_pid(), &status, 0);
        }
    }
    close(timer_fd);
    close(signal_fd);
    return 0;
}

} // namespace waylaunch
