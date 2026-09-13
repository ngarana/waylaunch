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
#include "waylaunch/wayland_core.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// Hyprland announces nothing when a window is resized or moved — socket2 is
// silent through a whole drag (verified live on 0.56.2) — so the strip cannot
// be event-driven onto the terminal it belongs to. It samples instead: this is
// the poll timeout while the dropdown is on screen, fast enough to look
// attached during a drag and costing one j/clients read per tick. While hidden
// the loop idles on the slower config-mtime cadence.
constexpr int kVisiblePollMs = 120;
constexpr int kIdlePollMs = 500;

// A tab bar showing one tab says nothing: the active-tab highlight fills the
// whole strip because it has no siblings to contrast against, which reads as a
// solid accent-coloured slab rather than a tab. Below this count the strip
// stays down and the terminal takes the band back — the same rule yakuake and
// kitty use.
constexpr size_t kMinTabsForStrip = 2;

// Cooldown before rebuilding the strip's Wayland connection after it fails or
// dies, so a compositor that is restarting is not re-probed on every toggle.
constexpr int kWaylandRetrySec = 5;

// Ignore sub-threshold geometry drift when learning a user resize: compositor
// rounding and border/gap arithmetic move the reported size by a pixel or two
// without the user having touched anything.
constexpr int kResizeEpsilonPx = 8;

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

} // namespace

int dropdown_main(const std::string& slot, const std::string& config_path) {
    SessionSupervisor supervisor(slot);
    DropdownManager manager;
    HyprlandBackend backend(slot);

    // Section defaults plus this slot's overrides (§6). Unknown slot names
    // yield the section untouched, so ad-hoc `--dropdown scratch` works.
    // Reloadable (§6.1): kept in mutable locals refreshed by maybe_reload()
    // so edits apply without restarting the daemon.
    waylaunch::Config repo_config;
    const std::string path =
        config_path.empty() ? waylaunch::Config::default_config_path() : config_path;
    DropdownConfig config;
    std::string slot_command;
    bool dropdown_enabled = true;
    std::optional<std::filesystem::file_time_type> config_mtime;
    DropdownStateStore state_store;

    FocusGuard guard;
    HyprlandEventStream events;
    bool backend_usable = backend.supports_geometry();
    // Last address the slot window was seen at. closewindow arrives after the
    // window is already gone from j/clients, so identity must come from here
    // rather than a fresh lookup.
    std::string slot_address;

    // Phase 5 tab strip: first Wayland in this daemon. Lazily initialized on
    // first show so lifecycle-only (and non-Wayland) environments never pay
    // for it; dormant (unmapped) while hidden.
    // Rebuildable so a protocol error costs the strip only until the next
    // show, not for the daemon's life.
    std::unique_ptr<WaylandCore> wayland;
    Renderer strip_renderer;
    TabStrip tab_strip;
    bool strip_ready = false;
    bool strip_needs_render = false;
    int strip_rendered_width = 0; // last painted width; hit-testing coordinate space
    // The rect the strip is currently mapped to, and the working-area origin
    // its margins were computed against. Compared each visible tick against
    // where the terminal actually is, so a live resize drags the strip along
    // instead of stranding it at the placement-time geometry.
    std::optional<Geometry> strip_rect;
    int strip_working_top = 0;
    int strip_working_left = 0;
    // Earliest retry after a Wayland teardown; keeps a compositor that is
    // still coming back from being hammered once per toggle.
    std::chrono::steady_clock::time_point wayland_retry_after{};

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

    // This slot's windows, for the strip — read from j/clients, the same
    // source placement uses, so tab membership obeys the pid-ownership rule
    // and a hand-spawned same-class window never shows up as a tab.
    auto collect_tabs = [&]() {
        std::vector<TabStrip::Tab> tabs;
        if (backend_usable) {
            for (const WindowInfo& window :
                 backend.find_owned_windows(supervisor.app_id(), supervisor.child_pid())) {
                tabs.push_back(
                    {.address = window.address, .title = window.title, .is_active = window.active});
            }
        }
        tab_strip.update(std::move(tabs));
    };

    auto render_strip = [&]() {
        if (!strip_ready || !wayland || !wayland->is_configured()) return;
        if (manager.current_state() != DropdownState::Visible) return;
        collect_tabs();
        Buffer* buf = wayland->acquire_buffer();
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
        wayland->submit_buffer(buf, 0, 0);
    };

    auto ensure_strip = [&]() -> bool {
        if (!config.tab_strip || !backend_usable) return false;
        if (strip_ready) return true;
        if (std::chrono::steady_clock::now() < wayland_retry_after) return false;
        wayland = std::make_unique<WaylandCore>();
        // Namespace and output are fixed at surface creation, so seed the
        // config before init(); remap_surface() only re-applies
        // anchor/size/margins/interactivity/zone (and rebuilds the surface
        // when the target output changes).
        // NOTE: this pre-seed must stay protocol-legal on its own (Hyprland
        // kills surfaces committed with a zero size on a partial anchor
        // set), so it keeps all four anchors with a zero span.
        wayland->set_want_backdrop(false);
        {
            LayerSurfaceConfig initial;
            initial.keyboard = LayerKeyboardMode::None;
            initial.exclusive_zone = 0;
            initial.layer_namespace = "waylaunch-dropdown-tabs";
            wayland->set_layer_surface_config(initial);
        }
        if (!wayland->init()) {
            wayland.reset();
            // Back off before the next attempt so a compositor that is down
            // is not re-probed on every toggle.
            wayland_retry_after =
                std::chrono::steady_clock::now() + std::chrono::seconds(kWaylandRetrySec);
            return false;
        }
        wayland->set_mouse_handler([&](double x, double y, uint32_t button, bool pressed) {
            if (!pressed || button != kBtnLeft) return;
            if (manager.current_state() != DropdownState::Visible || !strip_ready) return;
            std::string target =
                tab_strip.hit_test(static_cast<int>(x), static_cast<int>(y), strip_rendered_width);
            if (target.empty()) return;
            if (auto window = backend.find_by_address(target); window.has_value()) {
                backend.focus(*window);
            }
        });
        wayland->set_redraw_handler([&]() { strip_needs_render = true; });
        wayland->set_close_handler([&]() {
            strip_ready = false;
            strip_rect.reset();
        });
        strip_ready = true;
        return true;
    };

    // Map the strip flush above the placed terminal and paint it.
    // Margins are relative to the layer working area (output origin pushed
    // past other exclusive zones like the bar), NOT to the output origin —
    // verified live: a margin of reserved_top landed the strip a full bar
    // height too low. working_top/left anchor the computation.
    auto show_strip = [&](const Geometry& rect, int working_top, int working_left,
                          const std::string& output_name) {
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
        // Pin the strip to the same monitor the terminal was just placed on.
        // Without this the compositor picks the output under the pointer, and
        // on multi-head the strip and its terminal drift apart.
        layer.output_name = output_name;
        strip_rect = rect;
        strip_working_top = working_top;
        strip_working_left = working_left;
        wayland->set_layer_surface_config(layer);
        wayland->remap_surface();
        // Collect the configure synchronously (the switcher's map-NOW pattern):
        // without this the first show would wait for the next event. Bounded:
        // an unanswered sync must degrade to a later paint, never wedge us.
        wl_display* dpy = wayland->display();
        if (dpy != nullptr) display_roundtrip_bounded(dpy, 1000);
        strip_needs_render = true;
        render_strip();
    };

    auto hide_strip = [&]() {
        strip_rect.reset();
        if (strip_ready && wayland) wayland->unmap_surface();
    };

    auto spawn = [&]() {
        // Slot command wins; otherwise [dropdown].terminal/probe, so the
        // window always carries the slot app-id for find_window.
        std::vector<std::string> argv =
            slot_command.empty()
                ? SessionSupervisor::build_argv(config.terminal, supervisor.app_id())
                : SessionSupervisor::build_slot_argv(slot_command, supervisor.app_id());
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
    // Size we last asked the terminal to take, and the band the strip stole
    // from it, so a user resize can be told apart from our own placement.
    std::optional<Geometry> placed_size;
    int placed_strip_band = 0;
    // Owned-window count as of the last placement. Compared against the live
    // count so that crossing the one-tab threshold re-places the terminal
    // (the band appears or is handed back), while a Wayland outage that keeps
    // the strip down never looks like a crossing and so never re-places.
    size_t placed_tab_count = 0;

    // Persist a user's manual resize so the next show honours it (gap 5's
    // remaining half). The compositor is the source of truth: whatever the
    // window measures when we take it off-screen becomes the new override,
    // with the strip's band added back so the stored size is the whole
    // dropdown rather than just the terminal beneath it.
    auto note_resize = [&](const WindowInfo& window) {
        if (!placed_size.has_value()) return;
        auto learned = learn_resize(*placed_size, window.geom, placed_strip_band, kResizeEpsilonPx,
                                    window.floating);
        if (!learned.has_value()) return;
        config.size_override = learned;
        state_store.save_slot(slot, DropdownSlotState{.w = learned->w, .h = learned->h});
        // Re-baseline so the same resize is not re-learned on the next hide.
        placed_size = Geometry{.x = 0, .y = 0, .w = window.geom.w, .h = window.geom.h};
    };

    auto place_visible = [&]() -> std::optional<WindowInfo> {
        auto window = backend.find_window(supervisor.app_id(), supervisor.child_pid());
        auto monitor = backend.focused_monitor();
        if (!window.has_value() || !monitor.has_value()) return std::nullopt;
        Geometry base = compute_geometry(*monitor, config);
        Geometry term = base;
        // The strip earns its band only once there is a second tab to choose
        // between; below that the terminal gets the full dropdown height.
        size_t tab_count =
            backend.find_owned_windows(supervisor.app_id(), supervisor.child_pid()).size();
        bool strip =
            config.tab_strip && backend_usable && tab_count >= kMinTabsForStrip && ensure_strip();
        if (strip) {
            term.y += TabStrip::kHeight;
            term.h = std::max(1, term.h - TabStrip::kHeight);
        } else {
            // Covers both "never had one" and "just dropped to a single tab".
            hide_strip();
        }
        if (!backend.show(*window, term)) return std::nullopt;
        placed_size = Geometry{.x = 0, .y = 0, .w = term.w, .h = term.h};
        placed_strip_band = strip ? TabStrip::kHeight : 0;
        placed_tab_count = tab_count;
        if (strip) {
            show_strip(Geometry{.x = base.x, .y = base.y, .w = term.w, .h = TabStrip::kHeight},
                       monitor->y + monitor->reserved_top, monitor->x, monitor->name);
        }
        return window;
    };

    // Keep the strip glued to the terminal after placement. Hyprland emits no
    // event for a resize or a move, so the only way to notice the user
    // reshaping the dropdown is to look: each visible tick compares where the
    // terminal actually is against where the strip was last mapped, and
    // re-maps only on a real difference (so a steady dropdown costs one
    // j/clients read per tick and no Wayland traffic at all). Without this the
    // strip stays frozen at its placement-time width and hangs off the side of
    // a narrowed terminal until the next toggle.
    //
    // The same tick also watches the tab count, because gaining or losing the
    // strip changes how much height the terminal gets and so is a full
    // re-placement rather than a strip tweak.
    auto sync_strip_to_window = [&]() {
        if (manager.current_state() != DropdownState::Visible || !backend_usable) return;
        // One read serves both jobs: the count decides whether the strip
        // belongs on screen at all, and the matching entry carries the live
        // geometry to glue it to.
        std::vector<WindowInfo> owned =
            backend.find_owned_windows(supervisor.app_id(), supervisor.child_pid());
        if ((owned.size() >= kMinTabsForStrip) != (placed_tab_count >= kMinTabsForStrip)) {
            if (auto shown = place_visible(); shown.has_value()) {
                slot_address = shown->address;
                guard.set_slot(supervisor.child_pid(), slot_address);
                // show() refocuses, so restart the grace window rather than
                // let our own focus event read as the user leaving.
                guard.note_shown(std::chrono::steady_clock::now());
            }
            return;
        }
        if (!config.tab_strip || !strip_ready) return;
        if (!strip_rect.has_value()) return; // never mapped; place_visible owns the first map
        const WindowInfo* window = nullptr;
        for (const WindowInfo& candidate : owned) {
            if (!candidate.visible) continue;
            if (window == nullptr) window = &candidate;
            if (!slot_address.empty() &&
                normalize_address(candidate.address) == normalize_address(slot_address)) {
                window = &candidate;
                break;
            }
        }
        if (window == nullptr) return;
        // The strip sits in the band immediately above the terminal, clamped
        // to the working area: dragged to the very top there is nowhere left
        // to put it, and overlapping the terminal's first row beats covering
        // the bar.
        Geometry want{.x = window->geom.x,
                      .y = std::max(strip_working_top, window->geom.y - TabStrip::kHeight),
                      .w = window->geom.w,
                      .h = TabStrip::kHeight};
        if (want.x == strip_rect->x && want.y == strip_rect->y && want.w == strip_rect->w) return;
        auto monitor = backend.focused_monitor();
        if (!monitor.has_value()) return;
        show_strip(want, monitor->y + monitor->reserved_top, monitor->x, monitor->name);
    };

    auto park_hidden = [&]() -> std::optional<WindowInfo> {
        auto window = backend.find_window(supervisor.app_id(), supervisor.child_pid());
        if (!window.has_value()) return std::nullopt;
        // Measure before hiding: once the window is on the special workspace
        // its reported geometry is no longer what the user shaped. Gate on the
        // window's own visibility, not the manager's — on_toggle() advances
        // the state machine before calling this, while the focus-retract path
        // advances it after, and only the compositor's view is true on both.
        // A freshly spawned window is visible here too, but it has no
        // placement to compare against, so note_resize() no-ops on it.
        if (window->visible) note_resize(*window);
        if (!backend.hide(*window)) return std::nullopt;
        return window;
    };

    // Reconcile manager state with compositor truth. Used on toggle paths;
    // returns false when there is no window to act on.
    auto sync_presence = [&]() {
        if (backend_usable &&
            backend.find_window(supervisor.app_id(), supervisor.child_pid()).has_value()) {
            return true;
        }
        if (!supervisor.has_child() && manager.current_state() != DropdownState::Absent) {
            manager.process_event(DropdownEvent::WindowClosed);
        }
        return false;
    };

    // Drop the strip's Wayland connection entirely and schedule a rebuild.
    // Used when the display is poisoned by a protocol error or the compositor
    // goes away: the next show reconnects instead of the strip staying off
    // for the life of the daemon.
    auto shed_wayland = [&](const char* why) {
        std::fprintf(stderr, "waylaunch-dropdown: %s, rebuilding tab strip in %ds\n", why,
                     kWaylandRetrySec);
        strip_ready = false;
        strip_needs_render = false;
        strip_rect.reset();
        wayland.reset();
        wayland_retry_after =
            std::chrono::steady_clock::now() + std::chrono::seconds(kWaylandRetrySec);
    };

    // Hot-reload (§6.1): re-read the config file when its mtime moves and
    // apply without restarting. Parse failures and unreadable files keep the
    // running config — never set_defaults() over live state. Geometry keys
    // re-place immediately when visible; terminal/command apply to the next
    // spawn (a live process cannot be re-executed).
    auto reload_config = [&](bool force) {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) return; // unreadable (yet): keep running config
        if (!force && config_mtime.has_value() && *config_mtime == mtime) return;
        config_mtime = mtime;
        waylaunch::Config fresh;
        if (!fresh.load(path)) {
            if (force) std::cerr << "Warning: Could not load config, using dropdown defaults.\n";
            return;
        }
        repo_config = fresh; // copy: Config declares ctor/dtor, so no move assign
        const DropdownConfig& section = repo_config.get().dropdown;
        bool was_enabled = dropdown_enabled;
        dropdown_enabled = section.enabled;
        if (!dropdown_enabled) {
            if (was_enabled && manager.current_state() == DropdownState::Visible &&
                park_hidden().has_value()) {
                manager.process_event(DropdownEvent::WindowHidden);
                hide_strip();
            }
            return;
        }
        ResolvedSlot resolved = resolve_dropdown_slot(section, slot);
        config = resolved.config;
        slot_command = resolved.command;
        auto states = state_store.load();
        if (auto it = states.find(slot); it != states.end()) {
            config.size_override = Geometry{.x = 0, .y = 0, .w = it->second.w, .h = it->second.h};
        }
        supervisor.set_respawn_enabled(config.respawn);
        guard.configure(config.hide_on_focus_loss, config.focus_grace_ms);
        if (manager.current_state() == DropdownState::Visible && backend_usable) {
            if (auto shown = place_visible(); shown.has_value()) {
                slot_address = shown->address;
                guard.set_slot(supervisor.child_pid(), slot_address);
                guard.note_shown(std::chrono::steady_clock::now());
            }
            // On failure the stale geometry simply survives until the next
            // toggle; never hide a visible dropdown over a reload.
        }
    };

    auto on_toggle = [&]() {
        if (!dropdown_enabled) return; // off switch: idle until re-enabled
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
    reload_config(true);
    if (!dropdown_enabled) {
        std::cout << "waylaunch: dropdown overlay disabled ([dropdown].enabled is false)\n";
        return 0;
    }
    manager.process_event(DropdownEvent::Toggle);
    spawn();

    bool running = true;
    while (running) {
        reload_config(false);
        events.ensure_connected(std::chrono::steady_clock::now());
        // Wayland dispatch around poll (launcher pattern): prepare before
        // blocking, read-or-cancel after. Only while the strip is up.
        wl_display* wl_dpy = (wayland && strip_ready) ? wayland->display() : nullptr;
        if (wl_dpy != nullptr && wl_display_get_error(wl_dpy) != 0) {
            // A protocol error poisons the connection: prepare_read would
            // spin forever and signals would never be serviced (all blocked
            // for signalfd). Drop it and schedule a rebuild — lifecycle and
            // placement never depended on Wayland, and the next show past the
            // cooldown reconnects, so a compositor restart costs the strip
            // one toggle rather than the rest of the session.
            shed_wayland("wayland protocol error");
            wl_dpy = nullptr;
        }
        bool reading = false;
        if (wl_dpy != nullptr) {
            // Bounded: a never-draining queue must still reach poll() so
            // signals stay serviced.
            for (int i = 0; i < 100; ++i) {
                if (wl_display_prepare_read(wl_dpy) == 0) {
                    reading = true;
                    break;
                }
                if (wl_display_dispatch_pending(wl_dpy) < 0) break;
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
        // Bounded wait so the config mtime is rechecked while idle and the
        // strip can sample the terminal's geometry while visible; all event
        // sources remain level-triggered so nothing is lost.
        bool visible = manager.current_state() == DropdownState::Visible;
        int n = poll(fds, 4, visible ? kVisiblePollMs : kIdlePollMs);
        // Close the read window HERE, before any handler runs. libwayland's
        // prepare_read/read_events pair is a reader lock: a second
        // prepare_read on the same thread makes read_events wait for a peer
        // that does not exist, and the daemon parks in futex forever. The
        // handlers below re-enter libwayland — show_strip() round-trips to
        // collect the strip's configure — so holding the read across them
        // wedged the daemon on the second show, deaf to SIGUSR1 and SIGTERM
        // alike. Everything Wayland-facing now happens outside the window.
        bool wayland_lost = false;
        if (reading) {
            if (n > 0 && (fds[3].revents & POLLIN) != 0) {
                // A dead socket means the compositor went away. That is the
                // strip's problem alone: shedding beats taking the daemon
                // down and losing the session with it.
                if (wl_display_read_events(wl_dpy) < 0) wayland_lost = true;
            } else {
                wl_display_cancel_read(wl_dpy);
            }
            if (!wayland_lost && wl_display_dispatch_pending(wl_dpy) < 0) wayland_lost = true;
        }
        if (wayland_lost) {
            shed_wayland("wayland connection lost");
            wl_dpy = nullptr;
        }
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
                    // The active tab's pill moves with focus.
                    strip_needs_render |= strip_ready;
                } else if (event.name == "closewindow") {
                    on_close_event(event.payload);
                    strip_needs_render |= strip_ready;
                } else if (event.name == "openwindow" || event.name == "windowtitlev2" ||
                           event.name == "movewindowv2") {
                    // Tabs come from j/clients now, so membership and titles
                    // have to be re-read when the window set changes; these
                    // are the events that say it did.
                    strip_needs_render |= strip_ready;
                }
                // focusedmon: monitor following already happens through the
                // focused-monitor read on every show; while visible we
                // deliberately do not chase, so user drags are never fought.
            }
        }
        // Sample after the handlers, so a toggle in this same tick settles
        // first and the strip is never re-mapped onto a window that is on its
        // way out. Safe here for the same reason painting is: the read window
        // closed above.
        sync_strip_to_window();
        // Painting is safe here: the read window closed above, and a handler
        // may have shed the connection in the meantime.
        if (wayland && strip_ready && strip_needs_render) {
            strip_needs_render = false;
            render_strip();
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
