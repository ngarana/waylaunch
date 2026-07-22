#include "waylaunch/launcher_ui.h"
#include "waylaunch/wayland_core.h"
#include "waylaunch/renderer.h"
#include "waylaunch/config.h"
#include "waylaunch/clipboard.h"
#include "waylaunch/calculator.h"
#include "waylaunch/app_launcher.h"
#include "waylaunch/subprocess.h"
#include "waylaunch/content/store.h"
#include "waylaunch/content/config.h"
#include "waylaunch/switcher/wlr_toplevel_backend.h"
#include "waylaunch/switcher/app_switcher_manager.h"
#include "waylaunch/switcher/switcher_input_controller.h"
#include "waylaunch/switcher/switcher_renderer.h"
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <sstream>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <poll.h>

namespace waylaunch {

namespace {

bool ui_dbg() { static bool v = std::getenv("WAYLAUNCH_DEBUG") != nullptr; return v; }

// Signal → event-loop wakeup. A SIGINT/SIGTERM handler writes to this eventfd so
// run()'s poll loop can exit cleanly. Static because a handler carries no state;
// there is only ever one LauncherUI.
int g_signal_fd = -1;
void on_terminate_signal(int) {
    if (g_signal_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(g_signal_fd, &one, sizeof(one));   // async-signal-safe
        (void)w;
    }
}

// A re-invocation of the switcher signals the resident instance rather than
// stacking a new window: SIGUSR1 = forward (show if dormant, else advance),
// SIGUSR2 = reverse (show-at-far-end if dormant, else step back).
int g_switcher_advance_fd = -1;
void on_switcher_advance_signal(int) {
    if (g_switcher_advance_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(g_switcher_advance_fd, &one, sizeof(one));
        (void)w;
    }
}

int g_switcher_reverse_fd = -1;
void on_switcher_reverse_signal(int) {
    if (g_switcher_reverse_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(g_switcher_reverse_fd, &one, sizeof(one));
        (void)w;
    }
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string(".");
}

// Replace a leading $HOME with "~" for compact display.
std::string abbreviate_home(const std::string& path) {
    std::string h = home_dir();
    if (!h.empty() && path.rfind(h, 0) == 0) return "~" + path.substr(h.size());
    return path;
}

// Expand a leading "~" (or "~/") to $HOME.
std::string expand_tilde(const std::string& path) {
    if (path == "~") return home_dir();
    if (path.rfind("~/", 0) == 0) return home_dir() + path.substr(1);
    return path;
}

// Percent-encode a filesystem path for use in a file:// URI. Keeps the
// unreserved set (RFC 3986) plus '/' so directory separators stay readable.
std::string percent_encode_path(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// Map a file extension to a freedesktop icon name (falls back to a monogram
// if the theme has no such icon).
std::string icon_for_file(const std::string& path) {
    std::string ext = to_lower(std::filesystem::path(path).extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
        ext == ".svg" || ext == ".webp" || ext == ".bmp") return "image-x-generic";
    if (ext == ".pdf") return "application-pdf";
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a") return "audio-x-generic";
    if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".mov") return "video-x-generic";
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".7z" || ext == ".rar") return "package-x-generic";
    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" || ext == ".py" ||
        ext == ".js" || ext == ".ts" || ext == ".rs" || ext == ".go" || ext == ".java" ||
        ext == ".sh") return "text-x-script";
    return "text-x-generic";
}

std::string format_size(off_t bytes) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    char buf[64];
    if (i == 0) snprintf(buf, sizeof(buf), "%lld %s", static_cast<long long>(bytes), u[i]);
    else        snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

std::string format_time(time_t t) {
    char buf[64];
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%b %d, %Y  %H:%M", &tmv);
    return buf;
}

// Content snippets carry the matched runs wrapped in these sentinel bytes (the
// hl markers passed to Store::search). They're control chars the extractor's
// sanitizer strips from body text, so they never collide with real content.
constexpr char kHlOpen = '\x02';
constexpr char kHlClose = '\x03';

// Escape text for Pango markup (so a stray '&'/'<' in a filename or excerpt
// can't corrupt the markup we build around highlighted runs).
std::string escape_markup(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string color_hex(const Color& c) {
    auto b = [](double v) { return static_cast<int>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    return buf;
}

// Turn a sentinel-marked snippet into Pango markup: plain text is escaped, and
// each matched run becomes an accent-coloured bold span.
std::string snippet_markup(const std::string& snip, const std::string& accent_hex) {
    std::string out;
    size_t i = 0;
    while (i < snip.size()) {
        size_t o = snip.find(kHlOpen, i);
        if (o == std::string::npos) { out += escape_markup(snip.substr(i)); break; }
        out += escape_markup(snip.substr(i, o - i));
        size_t c = snip.find(kHlClose, o + 1);
        if (c == std::string::npos) { out += escape_markup(snip.substr(o + 1)); break; }
        out += "<span foreground=\"" + accent_hex + "\" weight=\"bold\">";
        out += escape_markup(snip.substr(o + 1, c - o - 1));
        out += "</span>";
        i = c + 1;
    }
    return out;
}

// Short uppercase type tag for the preview badge (PDF, XLSX, FOLDER, APP…).
std::string kind_badge(const ListItem& it) {
    switch (it.kind) {
        case ItemKind::Application: return "APP";
        case ItemKind::Folder:      return "FOLDER";
        case ItemKind::Calculator:  return "=";
        case ItemKind::Command:     return "CMD";
        default: break;
    }
    std::string ext = to_lower(std::filesystem::path(it.path).extension().string());
    if (ext.size() > 1) {
        ext = ext.substr(1);
        for (auto& ch : ext) ch = static_cast<char>(std::toupper((unsigned char)ch));
        return ext.size() <= 5 ? ext : ext.substr(0, 5);
    }
    return "FILE";
}

int path_depth(const std::string& p) {
    return static_cast<int>(std::count(p.begin(), p.end(), '/'));
}

// More recently modified files rank higher (small bonus).
float recency_bonus(time_t mtime) {
    double days = (std::time(nullptr) - mtime) / 86400.0;
    if (days < 1)   return 60.0f;
    if (days < 7)   return 40.0f;
    if (days < 30)  return 20.0f;
    if (days < 365) return 8.0f;
    return 0.0f;
}

// Ask the compositor for a frosted-glass backdrop behind our layer surface.
// A Wayland SHM client cannot blur what is behind its own surface, so on
// Hyprland we enable its layer-shell blur for our namespace ("waylaunch").
// Returns true if blur was requested (→ the panel is drawn as translucent glass).
bool try_enable_backdrop_blur() {
    if (std::getenv("WAYLAUNCH_NO_BLUR")) return false;
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;   // Hyprland only
    if (!Subprocess::command_exists("hyprctl")) return false;
    // Only render as glass if the compositor is actually blurring — otherwise a
    // translucent panel is just unreadable. We never force global blur on (that
    // would change the user's whole desktop); they opt in via their Hyprland
    // config: `decoration:blur:enabled = true` plus `layerrule = blur, waylaunch`.
    auto r = Subprocess::run({"hyprctl", "getoption", "decoration:blur:enabled"});
    if (r.stdout.find("bool: true") == std::string::npos) return false;
    // Best-effort: register the layer blur rule for our namespace.
    Subprocess::run({"hyprctl", "keyword", "layerrule", "blur, waylaunch"});
    Subprocess::run({"hyprctl", "keyword", "layerrule", "ignorealpha 0.5, waylaunch"});
    return true;
}

// Launch a command fully detached from waylaunch (double-fork + setsid) using an
// argv vector, so arguments are never re-parsed by a shell. The intermediate
// child is reaped immediately; the grandchild is reparented to init.
void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            std::vector<char*> c_argv;
            c_argv.reserve(argv.size() + 1);
            for (const auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
            c_argv.push_back(nullptr);
            execvp(c_argv[0], c_argv.data());
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

} // namespace

LauncherUI::LauncherUI() = default;

LauncherUI::~LauncherUI() {
    {
        std::lock_guard<std::mutex> lk(file_mtx_);
        file_stop_ = true;
    }
    file_cv_.notify_all();
    if (file_thread_.joinable()) file_thread_.join();
    if (results_fd_ >= 0) close(results_fd_);
    g_signal_fd = -1;   // stop the handler before the fd goes away
    if (signal_fd_ >= 0) close(signal_fd_);
    g_switcher_advance_fd = -1;
    if (switcher_advance_fd_ >= 0) close(switcher_advance_fd_);
    g_switcher_reverse_fd = -1;
    if (switcher_reverse_fd_ >= 0) close(switcher_reverse_fd_);
}

bool LauncherUI::init(Config& config) {
    config_ = &config;

    // Apply panel geometry from [appearance].
    const auto& ap = config.get().appearance;
    layout_.win_w         = ap.width;
    layout_.margin_top    = ap.margin_top;
    layout_.corner_radius = ap.corner_radius;
    layout_.search_h      = ap.search_height;
    layout_.row_h         = ap.row_height;
    layout_.icon_size     = ap.icon_size;
    layout_.list_w        = ap.list_width;
    layout_.max_per_group = std::max(1, ap.max_per_group);

    // Backdrop blur: "off" disables glass entirely (opaque panel). Otherwise try
    // the compositor rule first, then fall back to client-side capture blur.
    // The switcher must appear instantly on Alt+Tab, so it skips the (slow,
    // synchronous) screen capture + blur entirely — its HUD floats over the
    // desktop without it.
    bool want_blur = (ap.blur != "off") && !switcher_mode_;
    bool compositor_blur = false;
    if (want_blur) compositor_blur = try_enable_backdrop_blur();

    wayland_ = std::make_unique<WaylandCore>();
    wayland_->set_want_backdrop(want_blur);

#ifdef HAS_FOREIGN_TOPLEVEL
    // Create the toplevel backend and register the manager listener BEFORE
    // wayland_->init(): the manager global is bound during init's registry
    // roundtrip, and the compositor emits the `toplevel` events for already-open
    // windows immediately after that bind. If the listener isn't in place yet,
    // those initial windows are missed and the switcher shows nothing.
    switcher_backend_ = std::make_unique<WlrForeignToplevelBackend>();
    switcher_backend_->set_activate_command(config.get().app_switcher.activate_command);
    wayland_->set_foreign_toplevel_listener([this](zwlr_foreign_toplevel_manager_v1* mgr) {
        if (switcher_backend_) switcher_backend_->bind_manager(mgr);
    });
#endif

    if (!wayland_->init()) return false;

    renderer_ = std::make_unique<Renderer>();

    blur_enabled_ = compositor_blur;
    if (want_blur && wayland_->has_backdrop()) {
        renderer_->set_backdrop(wayland_->backdrop_data(), wayland_->backdrop_width(),
                                wayland_->backdrop_height(), wayland_->backdrop_stride(),
                                wayland_->backdrop_format(), wayland_->backdrop_y_invert());
        if (renderer_->has_backdrop()) blur_enabled_ = true;
    }
    if (ui_dbg()) fprintf(stderr, "[ui] backdrop wl=%d renderer=%d blur_enabled=%d\n",
                          wayland_->has_backdrop(), renderer_->has_backdrop(), blur_enabled_);

    wayland_->set_key_handler([this](uint32_t k, uint32_t u, bool p) { on_key(k, u, p); });
    wayland_->set_modifiers_handler([this](uint32_t mods) {
        if (switcher_input_) switcher_input_->handle_modifiers(mods);
    });
    wayland_->set_mouse_handler([this](double x, double y, uint32_t b, bool p) { on_mouse(x, y, b, p); });
    wayland_->set_axis_handler([this](double x, double y, int32_t a, double v) { on_axis(x, y, a, v); });
    wayland_->set_close_handler([this]() { on_close(); });
    wayland_->set_redraw_handler([this]() { on_redraw(); });

#ifdef HAS_FOREIGN_TOPLEVEL
    // The backend + manager listener were set up before init() (above), so the
    // backend already holds the initial windows. Build the rest of the switcher,
    // which reads them via the manager's initial rebuild.
    if (wayland_->foreign_toplevel_manager_)
        switcher_backend_->bind_manager(wayland_->foreign_toplevel_manager_);  // fallback (no-op if already bound)
    switcher_manager_ = std::make_unique<AppSwitcherManager>(switcher_backend_.get());
    switcher_manager_->set_group_by_app(config.get().app_switcher.group_by_app);
    switcher_input_ = std::make_unique<SwitcherInputController>(switcher_manager_.get(), wayland_->seat_);
    switcher_renderer_ = std::make_unique<SwitcherRenderer>();

    switcher_manager_->set_change_callback([this]() {
        needs_redraw_ = true;
        // Resident switcher: the first Alt+Tab starts this process; it then stays
        // alive and warm. On show it maps + grabs the keyboard; on hide (confirm
        // or cancel) it unmaps — releasing the grab so the user's windows get the
        // keyboard back — but keeps running, so every later Alt+Tab (delivered as
        // SIGUSR1/SIGUSR2) shows instantly with a reliable grab.
        if (switcher_mode_) {
            if (switcher_manager_->is_visible()) {
                switcher_shown_ = true;
            } else if (switcher_shown_) {
                switcher_shown_ = false;
                wayland_->unmap_surface();   // go dormant; do NOT quit
            }
        }
    });
#endif

    // File-search settings from [search].
    const auto& sc = config.get().search;
    file_roots_.clear();
    for (const auto& r : sc.file_roots) if (!r.empty()) file_roots_.push_back(expand_tilde(r));
    if (file_roots_.empty()) {
        for (const auto& p : sc.paths)
            if (p.type != "desktop" && !p.path.empty()) file_roots_.push_back(expand_tilde(p.path));
    }
    if (file_roots_.empty()) file_roots_.push_back(home_dir());
    file_excludes_ = sc.file_excludes;
    if (file_excludes_.empty()) {
        file_excludes_ = {".git", "node_modules", ".cache", "target", ".venv",
                          "__pycache__", ".cargo", ".rustup", "go/pkg",
                          ".local/share/Trash"};
    }
    file_min_query_ = std::max(1, sc.file_min_query);
    max_file_results_ = std::max(1, sc.max_file_results);
    file_enabled_ = sc.enable_files;

    // Content search: open the waylaunchd index read-only. Absent/locked index →
    // content_store_ stays null and we silently degrade to filename search (NFR8).
    // Skipped in switcher mode (no search UI → no reason to open the index).
    if (!switcher_mode_) {
        content::ContentConfig cc = content::load_content_config(config_path_);
        content_enabled_ = cc.enable;
        content_min_query_ = std::max(1, cc.min_query);
        content_max_results_ = std::max(1, cc.max_results);
        if (content_enabled_) {
            auto store = std::make_unique<content::Store>();
            if (store->open(content::ContentConfig::db_path(), {true, cc.match}))
                content_store_ = std::move(store);
            else
                content_enabled_ = false;   // no index yet; degrade
        }
        if (ui_dbg())
            fprintf(stderr, "[ui] content search %s (min_query=%d)\n",
                    content_store_ ? "on" : "off", content_min_query_);
    }

    results_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    file_thread_ = std::thread([this]() { file_worker_loop(); });

    // Exit the poll loop cleanly on SIGINT/SIGTERM (not just on Esc).
    signal_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_signal_fd = signal_fd_;
    struct sigaction sa{};
    sa.sa_handler = on_terminate_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // The Alt+Tab bind re-invocation signals this resident instance (see main.cpp):
    // SIGUSR1 = forward (show if dormant, else advance), SIGUSR2 = reverse.
    switcher_advance_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_switcher_advance_fd = switcher_advance_fd_;
    struct sigaction su{};
    su.sa_handler = on_switcher_advance_signal;
    sigemptyset(&su.sa_mask);
    sigaction(SIGUSR1, &su, nullptr);

    switcher_reverse_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    g_switcher_reverse_fd = switcher_reverse_fd_;
    struct sigaction sr{};
    sr.sa_handler = on_switcher_reverse_signal;
    sigemptyset(&sr.sa_mask);
    sigaction(SIGUSR2, &sr, nullptr);

    if (!initial_query_.empty()) {
        query_ = initial_query_;
        cursor_pos_ = query_.size();
    }

    // The dedicated switcher never runs the search providers — skip scanning apps
    // and kicking the file/content workers (pure waste that also delays showing
    // the overlay).
    if (!switcher_mode_) {
        scan_apps();
        update_search();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Main loop: poll the Wayland fd AND the worker's eventfd, so asynchronous file
// results wake us to repaint instead of being rendered off-thread.
// ---------------------------------------------------------------------------
void LauncherUI::run() {
    wayland_->set_running(true);
    wl_display* dpy = wayland_->display();
    int wlfd = wl_display_get_fd(dpy);

    // Show (or reverse-show) the switcher: activate the state machine and preselect
    // the previous app — or the far end for a reverse cycle. Used for both the first
    // show and every warm re-show triggered by a later Alt+Tab.
    auto show_switcher = [&](bool reverse) {
        if (!switcher_input_ || !switcher_manager_) return;
        // Waking from dormancy: the unmap discarded the layer-surface state, so
        // start the re-map handshake. Rendering stays blocked (is_configured()
        // false) until the fresh configure arrives, whose redraw callback then
        // paints and maps in the same loop iteration.
        if (!wayland_->is_configured()) wayland_->remap_surface();
        switcher_input_->trigger();   // Hidden → active; show() preselects index 1
        if (reverse) {
            size_t nn = switcher_manager_->app_groups().size();
            if (nn > 1) switcher_manager_->jump_to(nn - 1);
        }
        needs_redraw_ = true;
    };

    // Dedicated Alt+Tab overlay: let the layer surface configure and the
    // foreign-toplevel manager announce the existing windows (handle + app_id/
    // title/state), then show the switcher with the previous app preselected. The
    // process then stays resident (see the change callback): it unmaps on confirm
    // but keeps tracking windows, so later Alt+Tabs re-show it instantly.
    if (switcher_mode_ && switcher_input_) {
        wl_display_roundtrip(dpy);
        wl_display_roundtrip(dpy);
        show_switcher(switcher_reverse_);
        // Map NOW, before entering the loop: poll() below blocks until an fd
        // event arrives, but an unmapped surface gets no events from the
        // compositor — so without this render the first show would sit
        // invisible until the *next* Alt+Tab's SIGUSR1 woke the loop (the
        // "press Tab twice to see it" bug).
        render_frame();
    }

    struct pollfd fds[5];
    fds[0].fd = wlfd;               fds[0].events = POLLIN;
    fds[1].fd = results_fd_;        fds[1].events = POLLIN;
    fds[2].fd = signal_fd_;         fds[2].events = POLLIN;
    fds[3].fd = switcher_advance_fd_; fds[3].events = POLLIN;
    fds[4].fd = switcher_reverse_fd_; fds[4].events = POLLIN;

    while (wayland_->is_running()) {
        // Canonical libwayland poll integration.
        while (wl_display_prepare_read(dpy) != 0)
            wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);

        for (auto& f : fds) f.revents = 0;
        int n = poll(fds, 5, -1);
        if (n < 0) {
            wl_display_cancel_read(dpy);
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(dpy) < 0) break;
        } else {
            wl_display_cancel_read(dpy);
        }
        wl_display_dispatch_pending(dpy);

        if (fds[1].revents & POLLIN) {
            uint64_t v;
            while (read(results_fd_, &v, sizeof(v)) == static_cast<ssize_t>(sizeof(v))) {}
            apply_file_results();
        }

        if (fds[3].revents & POLLIN) {   // SIGUSR1 → forward: show if dormant, else advance
            uint64_t v;
            while (read(switcher_advance_fd_, &v, sizeof(v)) == static_cast<ssize_t>(sizeof(v))) {}
            if (switcher_manager_) {
                if (switcher_manager_->is_visible()) switcher_manager_->navigate_next();
                else show_switcher(false);
            }
        }

        if (fds[4].revents & POLLIN) {   // SIGUSR2 → reverse: reverse-show if dormant, else step back
            uint64_t v;
            while (read(switcher_reverse_fd_, &v, sizeof(v)) == static_cast<ssize_t>(sizeof(v))) {}
            if (switcher_manager_) {
                if (switcher_manager_->is_visible()) switcher_manager_->navigate_prev();
                else show_switcher(true);
            }
        }

        if (fds[2].revents & POLLIN) {   // SIGINT/SIGTERM → exit cleanly
            quit();
            break;
        }

        if (needs_redraw_) render_frame();
    }

    // Flush any final requests (e.g. the switcher's window-activate) before the
    // process exits, since the loop stops without another flush pass.
    wl_display_flush(dpy);
}

void LauncherUI::quit() { wayland_->quit(); }

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

Theme LauncherUI::build_theme() const {
    const auto& tc = config_->get().theme;
    Theme t;
    t.background = Color::from_hex(tc.colors.background);
    t.background_alt = Color::from_hex(tc.colors.background_alt);
    t.foreground = Color::from_hex(tc.colors.foreground);
    t.text_muted = Color::from_hex(tc.colors.text_muted);
    t.accent = Color::from_hex(tc.colors.accent);
    t.accent_hover = Color::from_hex(tc.colors.accent_hover);
    t.error = Color::from_hex(tc.colors.error);
    t.warning = Color::from_hex(tc.colors.warning);
    t.success = Color::from_hex(tc.colors.success);
    t.border = Color::from_hex(tc.colors.border);
    t.selection = Color::from_hex(tc.colors.selection);
    t.corner_radius = tc.corner_radius;
    t.opacity = tc.opacity;
    auto to_rfc = [](const ConfigFont& cf) -> RenderFontConfig {
        RenderFontConfig r;
        r.family = cf.family;
        r.size = cf.size;
        r.bold = (cf.weight == "bold" || cf.weight == "Bold");
        r.italic = (cf.style == "italic" || cf.style == "Italic" || cf.style == "oblique");
        return r;
    };
    t.input_font = to_rfc(tc.input_font);
    t.result_font = to_rfc(tc.result_font);
    t.result_detail_font = to_rfc(tc.result_detail_font);
    return t;
}

// ---------------------------------------------------------------------------
// Unified search: applications + calculator (synchronous) merged with files
// (asynchronous). No modes — one query returns everything relevant, grouped,
// with a single Top Hit. This is the core difference from a rofi-style picker.
// ---------------------------------------------------------------------------

void LauncherUI::scan_apps() {
    apps_ = std::make_unique<AppLauncher>();
    std::vector<std::string> paths;
    for (auto& p : config_->get().search.paths) {
        if (p.type == "desktop") paths.push_back(p.path);
    }
    apps_->set_search_paths(paths);
    apps_->scan();   // once — filtering afterwards is in-memory
}

void LauncherUI::rebuild_app_items() {
    app_items_.clear();
    if (query_.empty()) return;   // search shows nothing until you type

    const auto& sc = config_->get().search;
    std::string q = to_lower(query_);

    // Calculator: a valid math expression becomes the Top Hit.
    if (sc.enable_calculator) {
        Calculator calc;
        auto r = calc.evaluate(query_);
        if (r.valid && calc.is_calculator_query(query_)) {
            ListItem it;
            it.kind = ItemKind::Calculator;
            it.name = "= " + r.result;
            it.description = query_;
            it.icon_name = "accessories-calculator";
            it.path = r.result;          // payload copied to clipboard on Return
            it.score = 1e6f;
            app_items_.push_back(std::move(it));
        }
    }

    // Custom commands from [[commands]], matched by name.
    if (sc.enable_commands) {
        for (const auto& cmd : config_->get().commands) {
            if (cmd.name.empty() || cmd.command.empty()) continue;
            std::string n = to_lower(cmd.name);
            size_t pos = n.find(q);
            if (pos == std::string::npos) continue;
            ListItem it;
            it.kind = ItemKind::Command;
            it.name = cmd.name;
            it.action_command = cmd.command;
            it.icon_name = cmd.icon.empty() ? "utilities-terminal" : cmd.icon;
            it.description = cmd.category.empty() ? "Command" : cmd.category;
            it.score = (pos == 0 ? 900.0f : 400.0f) - std::min<size_t>(pos, 200);
            app_items_.push_back(std::move(it));
        }
    }

    // Applications (in-memory over the cached scan), ranked by name match.
    if (sc.enable_applications) {
        std::vector<ListItem> apps;
        for (const auto& e : apps_->search(query_)) {
            ListItem it;
            it.kind = ItemKind::Application;
            it.name = e.name;
            it.path = e.exec;
            it.reveal_path = e.desktop_path;   // right-click → reveal the .desktop file
            it.description = e.comment.empty() ? e.generic_name : e.comment;
            it.icon_name = e.icon;
            std::string n = to_lower(e.name);
            size_t pos = n.find(q);
            if (pos == 0)                        it.score = 1000.0f - std::min<size_t>(n.size(), 500);
            else if (pos != std::string::npos)   it.score = 600.0f - std::min<size_t>(pos, 300);
            else                                 it.score = 50.0f;   // matched via comment/category
            apps.push_back(std::move(it));
        }
        std::stable_sort(apps.begin(), apps.end(),
                         [](const ListItem& a, const ListItem& b) { return a.score > b.score; });
        if (apps.size() > static_cast<size_t>(layout_.max_per_group))
            apps.resize(layout_.max_per_group);
        for (auto& a : apps) app_items_.push_back(std::move(a));
    }
}

void LauncherUI::rebuild_items() {
    // Combine every candidate, promote the single highest-scoring one to the
    // Top Hit (so a strong file match can win, not just apps), and keep the rest
    // in category order (applications, then files) for the grouped sections.
    std::vector<ListItem> af;
    af.reserve(app_items_.size() + file_items_.size());
    for (const auto& it : app_items_) af.push_back(it);
    for (const auto& it : file_items_) af.push_back(it);

    items_.clear();
    if (!af.empty()) {
        size_t best = 0;
        for (size_t i = 1; i < af.size(); ++i)
            if (af[i].score > af[best].score) best = i;
        items_.push_back(af[best]);
        for (size_t i = 0; i < af.size(); ++i)
            if (i != best) items_.push_back(af[i]);
        // Content matches always rank below app/file hits (Spotlight ordering),
        // in the store's BM25 order.
        for (const auto& it : content_items_) items_.push_back(it);
    } else {
        // Only content matched: it carries the hero row + CONTENTS section.
        for (const auto& it : content_items_) items_.push_back(it);
    }

    if (selected_index_ >= static_cast<int>(items_.size()))
        selected_index_ = std::max(0, static_cast<int>(items_.size()) - 1);
    relayout();
    needs_redraw_ = true;
}

// Recompute header/row positions (relative to the panel top) and the panel
// height, so rendering and hit-testing share one source of truth.
void LauncherUI::relayout() {
    rows_.clear();
    headers_.clear();

    if (items_.empty()) {
        panel_total_h_ = query_.empty() ? layout_.search_h
                                        : layout_.search_h + layout_.row_h;
        return;
    }

    // Category buckets so consecutive same-category rows share one header.
    auto category = [](ItemKind k) -> int {
        switch (k) {
            case ItemKind::Application:
            case ItemKind::Command:
            case ItemKind::Calculator: return 0;
            case ItemKind::File:
            case ItemKind::Folder:     return 1;
            case ItemKind::Content:    return 2;
        }
        return 1;
    };
    auto cat_label = [](int c) {
        return c == 0 ? "APPLICATIONS" : c == 2 ? "CONTENTS" : "FILES & FOLDERS";
    };

    int y = layout_.search_h + 8;
    headers_.push_back({"TOP HIT", y});
    y += layout_.header_h;
    rows_.push_back({0, y, true});
    y += layout_.row_h + 6;

    size_t i = 1;
    while (i < items_.size()) {
        int cat = category(items_[i].kind);
        headers_.push_back({cat_label(cat), y});
        y += layout_.header_h;
        while (i < items_.size() && category(items_[i].kind) == cat) {
            rows_.push_back({static_cast<int>(i), y, false});
            y += layout_.row_h;
            ++i;
        }
        y += 6;
    }

    int content = y + layout_.pad_x;
    panel_total_h_ = std::max(content, layout_.search_h + 240);
}

void LauncherUI::update_search() {
    selected_index_ = 0;
    scroll_offset_ = 0;
    rebuild_app_items();
    kick_file_search();     // clears file_items_ + schedules async search
    rebuild_items();
}

void LauncherUI::kick_file_search() {
    file_items_.clear();       // main-thread only
    content_items_.clear();
    // Bump the generation regardless so any in-flight worker result is dropped
    // as stale; schedule a run when either async provider (files/content) is on.
    bool enabled = file_enabled_ || (content_enabled_ && content_store_);
    {
        std::lock_guard<std::mutex> lk(file_mtx_);
        file_gen_++;
        if (enabled) {
            file_pending_query_ = query_;
            file_has_pending_ = true;
        }
    }
    if (enabled) file_cv_.notify_one();
}

void LauncherUI::apply_file_results() {
    std::vector<ListItem> got, got_content;
    {
        std::lock_guard<std::mutex> lk(file_mtx_);
        if (file_ready_gen_ != file_gen_) return;   // stale
        got = std::move(file_ready_);
        got_content = std::move(content_ready_);
        file_ready_.clear();
        content_ready_.clear();
    }
    file_items_ = std::move(got);
    content_items_ = std::move(got_content);
    rebuild_items();
}

// Worker thread: runs `fd` for the latest query and posts results back.
void LauncherUI::file_worker_loop() {
    for (;;) {
        std::string q;
        uint64_t gen;
        std::vector<std::string> roots;
        {
            std::unique_lock<std::mutex> lk(file_mtx_);
            file_cv_.wait(lk, [this] { return file_has_pending_ || file_stop_; });
            if (file_stop_) return;
            q = file_pending_query_;
            gen = file_gen_;
            file_has_pending_ = false;
            roots = file_roots_;
        }

        // Snapshot the (init-time constant) tuning so ranking below agrees with
        // the invocation.
        int min_query = std::max(1, file_min_query_);
        size_t keep = std::max(1, max_file_results_);

        std::vector<ListItem> out;
        if (file_enabled_ && static_cast<int>(q.size()) >= min_query &&
            Subprocess::command_exists("fd")) {
            // Match the query against the file *name*; scan a bounded number of
            // hits, then rank them and keep the best few. Noise directories are excluded .
            std::vector<std::string> argv = {
                "fd", "--color", "never", "--fixed-strings", "--max-results", "200",
                "--type", "f", "--type", "d"};
            for (const auto& ex : file_excludes_) {
                argv.push_back("--exclude");
                argv.push_back(ex);
            }
            argv.push_back(q);
            for (const auto& r : roots) argv.push_back(r);
            auto res = Subprocess::run(argv);

            std::string ql = to_lower(q);
            std::istringstream ss(res.stdout);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                struct stat st;
                bool have_stat = (stat(line.c_str(), &st) == 0);
                bool is_dir = have_stat ? S_ISDIR(st.st_mode) : false;

                ListItem it;
                it.kind = is_dir ? ItemKind::Folder : ItemKind::File;
                std::filesystem::path p(line);
                it.name = p.filename().string();
                if (it.name.empty()) it.name = line;
                it.path = line;
                it.description = abbreviate_home(line);
                it.icon_name = is_dir ? "folder" : icon_for_file(line);

                // Rank: prefix > substring on the name, shallower paths and more
                // recently modified files score higher.
                std::string nl = to_lower(it.name);
                size_t pos = nl.find(ql);
                float s;
                if (pos == 0)                        s = 1000.0f - std::min<size_t>(nl.size(), 300);
                else if (pos != std::string::npos)   s = 600.0f - std::min<size_t>(pos, 300);
                else                                 s = 200.0f;   // matched deeper in the path
                s -= path_depth(line) * 6.0f;
                if (is_dir) s += 15.0f;
                if (have_stat) s += recency_bonus(st.st_mtime);
                it.score = s;

                out.push_back(std::move(it));
            }

            std::stable_sort(out.begin(), out.end(),
                             [](const ListItem& a, const ListItem& b) { return a.score > b.score; });
            if (out.size() > keep) out.resize(keep);
        }

        // Content search: query the read-only index for body matches. Ranked by
        // BM25 (already ordered best-first by the store), shown in its own
        // CONTENTS section below apps/files.
        std::vector<ListItem> content_out;
        if (content_store_ && static_cast<int>(q.size()) >= content_min_query_) {
            auto hits = content_store_->search(q, content_max_results_,
                                               std::string(1, kHlOpen), std::string(1, kHlClose));
            for (auto& h : hits) {
                ListItem it;
                it.kind = ItemKind::Content;
                std::filesystem::path p(h.path);
                it.name = h.name.empty() ? p.filename().string() : h.name;
                it.path = h.path;
                it.reveal_path = h.path;
                it.description = abbreviate_home(h.path);
                it.snippet = h.snippet;
                it.icon_name = icon_for_file(h.path);
                it.score = static_cast<float>(h.score);
                content_out.push_back(std::move(it));
            }
        }

        {
            std::lock_guard<std::mutex> lk(file_mtx_);
            if (file_stop_) return;
            if (gen != file_gen_) continue;   // superseded by a newer query
            file_ready_ = std::move(out);
            content_ready_ = std::move(content_out);
            file_ready_gen_ = gen;
        }
        if (results_fd_ >= 0) {
            uint64_t one = 1;
            ssize_t w = write(results_fd_, &one, sizeof(one));
            (void)w;
        }
    }
}

// ---------------------------------------------------------------------------
// Selection & activation
// ---------------------------------------------------------------------------

int LauncherUI::panel_height() const { return panel_total_h_; }

void LauncherUI::select_item(int index) {
    if (items_.empty()) return;
    selected_index_ = std::max(0, std::min(index, static_cast<int>(items_.size()) - 1));
    needs_redraw_ = true;
}

void LauncherUI::autocomplete() {
    if (items_.empty()) return;
    const auto& it = items_[selected_index_];
    if (it.kind != ItemKind::Application) return;   // only app names make sense to complete
    query_ = it.name;
    cursor_pos_ = query_.size();
    update_search();
}

void LauncherUI::launch_selected() {
    if (items_.empty() || selected_index_ >= static_cast<int>(items_.size())) return;
    const auto& item = items_[selected_index_];
    if (ui_dbg()) fprintf(stderr, "[ui] activate kind=%d name='%s' path='%s'\n",
                          static_cast<int>(item.kind), item.name.c_str(), item.path.c_str());

    switch (item.kind) {
        case ItemKind::Calculator:
            if (!item.path.empty()) Clipboard::copy_text(item.path);
            break;
        case ItemKind::Command:
            if (!item.action_command.empty()) spawn_detached({"/bin/sh", "-c", item.action_command});
            break;
        case ItemKind::Application:
            // item.path is the .desktop Exec line — run it via a shell so
            // arguments/env prefixes work. It is a command, not a file.
            if (!item.path.empty()) spawn_detached({"/bin/sh", "-c", item.path});
            break;
        case ItemKind::File:
        case ItemKind::Folder:
        case ItemKind::Content:
            if (!item.path.empty()) {
                Clipboard::copy_file_path(item.path);
                spawn_detached({"xdg-open", item.path});
            }
            break;
    }
    quit();
}

void LauncherUI::open_file_location(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const auto& item = items_[index];

    // Choose what to reveal: files/folders reveal themselves; applications reveal
    // their .desktop file (the faithful "Show in Finder" for an app). The
    // calculator has nothing to reveal.
    std::string target;
    if (item.kind == ItemKind::File || item.kind == ItemKind::Folder ||
        item.kind == ItemKind::Content) target = item.path;
    else if (item.kind == ItemKind::Application) target = item.reveal_path;

    if (ui_dbg()) fprintf(stderr, "[ui] reveal idx=%d kind=%d target='%s'\n",
                          index, static_cast<int>(item.kind), target.c_str());
    if (target.empty()) return;

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(target, ec);
    if (ec) abs = target;
    if (!std::filesystem::exists(abs, ec)) return;

    // Preferred: freedesktop FileManager1.ShowItems (opens the folder AND selects
    // the file — the faithful "reveal in Finder" behaviour).
    if (Subprocess::command_exists("gdbus")) {
        std::string uri = "file://" + percent_encode_path(abs.string());
        auto r = Subprocess::run({
            "gdbus", "call", "--session",
            "--dest", "org.freedesktop.FileManager1",
            "--object-path", "/org/freedesktop/FileManager1",
            "--method", "org.freedesktop.FileManager1.ShowItems",
            "['" + uri + "']", ""});
        if (r.exit_code == 0) { quit(); return; }
    }

    // Fallback: open the enclosing directory with the default handler.
    std::string dir = abs.parent_path().string();
    if (dir.empty()) dir = ".";
    spawn_detached({"xdg-open", dir});
    quit();
}

// ---------------------------------------------------------------------------
// Keyboard / mouse
// ---------------------------------------------------------------------------

void LauncherUI::on_key(uint32_t keysym, uint32_t utf32, bool pressed) {
    if (ui_dbg()) fprintf(stderr, "[ui] on_key sym=0x%x utf32=%u pressed=%d\n", keysym, utf32, pressed);

    if (switcher_input_ && switcher_input_->is_active()) {
        if (switcher_input_->handle_key(keysym, pressed)) return;
    }

    bool alt = wayland_->kbd_.state && xkb_state_mod_name_is_active(
        wayland_->kbd_.state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE);
    bool super = wayland_->kbd_.state && xkb_state_mod_name_is_active(
        wayland_->kbd_.state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE);

    if ((alt || super) && keysym == XKB_KEY_Tab && pressed && switcher_input_) {
        switcher_input_->trigger();
        return;
    }

    if (!pressed) return;

    bool ctrl = wayland_->kbd_.state && xkb_state_mod_name_is_active(
        wayland_->kbd_.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);

    if (ctrl) {
        if (keysym == XKB_KEY_j || keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_k || keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
        if (keysym == XKB_KEY_n) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_p) { select_item(selected_index_ - 1); return; }
        if (keysym == XKB_KEY_v) {
            std::string pasted = Clipboard::paste_text();
            if (!pasted.empty()) {
                query_.insert(cursor_pos_, pasted);
                cursor_pos_ += pasted.size();
                update_search();
            }
            return;
        }
    }

    if (keysym == XKB_KEY_Return) { launch_selected(); return; }
    if (keysym == XKB_KEY_Escape) { quit(); return; }
    if (keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
    if (keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
    if (keysym == XKB_KEY_Tab) { autocomplete(); return; }
    if (keysym == XKB_KEY_Page_Up) { select_item(selected_index_ - layout_.max_per_group); return; }
    if (keysym == XKB_KEY_Page_Down) { select_item(selected_index_ + layout_.max_per_group); return; }
    if (keysym == XKB_KEY_Home) { select_item(0); return; }
    if (keysym == XKB_KEY_End) { select_item(static_cast<int>(items_.size()) - 1); return; }

    if (utf32 >= 32 && utf32 < 0x110000) {
        char utf8[5] = {0};
        if (utf32 < 0x80) { utf8[0] = static_cast<char>(utf32); }
        else if (utf32 < 0x800) { utf8[0] = 0xC0 | (utf32 >> 6); utf8[1] = 0x80 | (utf32 & 0x3F); }
        else if (utf32 < 0x10000) { utf8[0] = 0xE0 | (utf32 >> 12); utf8[1] = 0x80 | ((utf32 >> 6) & 0x3F); utf8[2] = 0x80 | (utf32 & 0x3F); }
        else { utf8[0] = 0xF0 | (utf32 >> 18); utf8[1] = 0x80 | ((utf32 >> 12) & 0x3F); utf8[2] = 0x80 | ((utf32 >> 6) & 0x3F); utf8[3] = 0x80 | (utf32 & 0x3F); }
        query_.insert(cursor_pos_, utf8);
        cursor_pos_ += std::strlen(utf8);
        update_search();
    } else if (keysym == XKB_KEY_BackSpace && cursor_pos_ > 0) {
        size_t prev = cursor_pos_;
        while (prev > 0 && (query_[--prev] & 0xC0) == 0x80) {}
        query_.erase(prev, cursor_pos_ - prev);
        cursor_pos_ = prev;
        update_search();
    } else if (keysym == XKB_KEY_Delete && cursor_pos_ < query_.size()) {
        size_t next = cursor_pos_;
        while (next < query_.size() && (query_[++next] & 0xC0) == 0x80) {}
        query_.erase(cursor_pos_, next - cursor_pos_);
        update_search();
    } else if (keysym == XKB_KEY_Left && cursor_pos_ > 0) {
        while (cursor_pos_ > 0 && (query_[--cursor_pos_] & 0xC0) == 0x80) {}
        needs_redraw_ = true;
    } else if (keysym == XKB_KEY_Right && cursor_pos_ < query_.size()) {
        while (cursor_pos_ < query_.size() && (query_[cursor_pos_] & 0xC0) == 0x80) cursor_pos_++;
        needs_redraw_ = true;
    }
}

void LauncherUI::on_mouse(double x, double y, uint32_t button, bool pressed) {
    if (ui_dbg()) fprintf(stderr, "[ui] on_mouse x=%.0f y=%.0f btn=0x%x pressed=%d\n", x, y, button, pressed);
    if (!pressed) return;

    constexpr uint32_t BTN_LEFT = 0x110;
    constexpr uint32_t BTN_RIGHT = 0x111;

    // Click anywhere outside the panel dismisses the launcher.
    if (button == BTN_LEFT) {
        int pw = layout_.win_w;
        int ph = panel_height();
        int px = (wayland_->surface_width() - pw) / 2;
        int py = layout_.margin_top;
        if (x < px || x > px + pw || y < py || y > py + ph) { quit(); return; }
    }

    int idx = hit_test(x, y);
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return;

    if (button == BTN_RIGHT) {
        select_item(idx);
        open_file_location(idx);
        return;
    }
    if (button != BTN_LEFT) return;
    if (idx == selected_index_) launch_selected();
    else select_item(idx);
}

void LauncherUI::on_axis(double, double, int32_t axis, double value) {
    if (axis == 0) {
        if (value < 0) select_item(selected_index_ - 3);
        else if (value > 0) select_item(selected_index_ + 3);
    }
}

void LauncherUI::on_close() { quit(); }

void LauncherUI::on_redraw() {
    needs_redraw_ = true;
    render_frame();
}

int LauncherUI::hit_test(double x, double y) const {
    int pw = layout_.win_w;
    int px = (wayland_->surface_width() - pw) / 2;
    int py = layout_.margin_top;

    if (x < px || x > px + pw || y < py || y > py + panel_total_h_) return -1;
    if (x > px + layout_.list_w) return -1;   // preview column isn't selectable
    for (const auto& r : rows_) {
        int ry = py + r.y;
        if (y >= ry && y < ry + layout_.row_h) return r.item_index;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void LauncherUI::render_frame() {
    if (!needs_redraw_) return;
    // A layer surface must not have a buffer attached before its first configure
    // is acked. Async file results (even the empty-query one kicked at startup)
    // can wake the loop before that happens — don't paint until we're configured.
    if (!wayland_->is_configured()) return;

    // Dedicated switcher overlay: do NOT attach a buffer (which maps the surface)
    // until the switcher is actually visible. Mapping a keyboard-exclusive layer
    // surface makes the compositor grab the keyboard and begin delivering modifier
    // events; if that happens before trigger(), a quick Alt+Tab's modifier-release
    // arrives while the state machine is still Hidden and is dropped — which forced
    // a second Tab. Staying unmapped until visible guarantees the grab, and every
    // modifier event, lands after trigger(). (After a confirm/cancel we hide then
    // quit, so no repaint is needed there either.)
    if (switcher_mode_ && !(switcher_manager_ && switcher_manager_->is_visible())) {
        needs_redraw_ = false;
        return;
    }

    Buffer* buf = wayland_->acquire_buffer();
    if (!buf) return;

    Theme t = build_theme();
    int bw = buf->width;
    int bh = buf->height;

    renderer_->begin(buf->data, buf->stride, bw, bh);
    renderer_->clear(Color::from_rgba(0, 0, 0, 0));   // transparent everywhere but the panel

    if (switcher_manager_ && switcher_manager_->is_visible()) {
        if (ui_dbg()) fprintf(stderr, "[sw] render %dx%d groups=%zu sel=%zu\n",
                              bw, bh, switcher_manager_->app_groups().size(),
                              switcher_manager_->selected_index());
        switcher_renderer_->render(*renderer_, *switcher_manager_, t, bw, bh);
        renderer_->end();
        wayland_->submit_buffer(buf, 0, 0);
        needs_redraw_ = false;
        return;
    }

    int ph = panel_height();
    int pw = layout_.win_w;
    int px = (bw - pw) / 2;
    int py = layout_.margin_top;

    // Drop shadow + panel. With compositor backdrop blur (Hyprland) the panel is
    // drawn as translucent frosted glass; without blur it stays mostly opaque so
    // text stays readable over busy backgrounds.
    const auto& ap = config_->get().appearance;
    double global_alpha = std::min(1.0, std::max(0.1, t.opacity));
    renderer_->rounded_rect(px - 2, py + 6, pw + 4, ph + 6, layout_.corner_radius + 2,
                            Color::from_rgba(0, 0, 0, 0.38 * global_alpha));
    Color panel = t.background;
    if (renderer_->has_backdrop()) {
        // Client-side frosted glass: the blurred desktop, clipped to the panel,
        // plus a translucent tint for text contrast.
        renderer_->draw_backdrop(px, py, pw, ph, layout_.corner_radius);
        renderer_->rounded_rect(px, py, pw, ph, layout_.corner_radius,
                                Color::from_rgba(panel.r, panel.g, panel.b, ap.backdrop_tint * global_alpha));
    } else {
        // No client backdrop: translucent if the compositor blurs, else opaque.
        double panel_a = (blur_enabled_ ? ap.panel_opacity : ap.opaque_opacity) * global_alpha;
        renderer_->rounded_rect(px, py, pw, ph, layout_.corner_radius,
                                Color::from_rgba(panel.r, panel.g, panel.b, panel_a));
    }
    // Glass rim: a soft light highlight along the top edge.
    if (blur_enabled_) {
        renderer_->fill_rect(px + layout_.corner_radius, py,
                             pw - 2 * layout_.corner_radius, 1,
                             Color::from_rgba(1, 1, 1, 0.22));
    } else {
        renderer_->fill_rect(px, py, pw, 1,
                             Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.6));
    }

    // --- Search field (spans the whole panel width) ---
    // Center the magnifier glyph and the text on one shared midline so they line
    // up. The glyph's visual mass sits ~0.41 down its box (lens + handle toward
    // the lower-right), and text is centered by its measured logical height.
    int midline = py + layout_.search_h / 2;
    int glyph_size = 26;
    int gx = px + layout_.search_pad_x;
    int gy = midline - static_cast<int>(glyph_size * 0.41);
    renderer_->draw_search_glyph(gx, gy, glyph_size,
                                 Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.9));

    int text_x = gx + glyph_size + 16;
    RenderFontConfig sf = t.input_font;
    sf.size = 24;
    int text_h = renderer_->text_height(sf);
    int text_y = midline - text_h / 2;
    if (query_.empty()) {
        renderer_->draw_text(text_x, text_y, config_->get().search.placeholder, sf,
                             Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.8));
    } else {
        renderer_->draw_text(text_x, text_y, query_, sf, Color::from_rgba(1, 1, 1, 1));
        int caret_x = text_x + renderer_->text_width(query_.substr(0, cursor_pos_), sf);
        int caret_h = text_h + 6;
        renderer_->fill_rect(caret_x + 1, midline - caret_h / 2, 2, caret_h, t.accent);
    }
    // --- Empty state: just the search bar (S---
    if (items_.empty()) {
        if (!query_.empty()) {
            renderer_->fill_rect(px, py + layout_.search_h, pw, 1,
                                 Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.35));
            int nw = renderer_->text_width("No Results", t.result_font);
            renderer_->draw_text(px + (pw - nw) / 2,
                                 py + layout_.search_h + layout_.row_h / 2 - 8,
                                 "No Results", t.result_font,
                                 Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.7));
        }
        renderer_->end();
        wayland_->submit_buffer(buf, 0, 0);
        needs_redraw_ = false;
        return;
    }

    // Divider under the search field (results present).
    renderer_->fill_rect(px, py + layout_.search_h, pw, 1,
                         Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.35));

    // Left result column bounds.
    int row_x = px + layout_.pad_x;
    int row_w = layout_.list_w - 2 * layout_.pad_x;

    std::string accent_hex = color_hex(t.accent);

    // Draw one result row (icon + name + subtitle). Hero rows get a larger icon.
    // Content rows show a highlighted excerpt as the subtitle instead of the path.
    auto draw_row = [&](const ListItem& it, int ry, bool sel, int icon) {
        if (sel) renderer_->rounded_rect(row_x - 6, ry - 2, row_w + 12, layout_.row_h, 10,
                                         Color::from_rgba(t.selection.r, t.selection.g, t.selection.b, 0.55));
        renderer_->draw_icon(row_x, ry + (layout_.row_h - icon) / 2, icon, it.icon_name, it.name, t.accent);
        int tx = row_x + icon + layout_.icon_pad;
        int avail = row_w - icon - layout_.icon_pad - 6;
        std::string name = it.name;
        if (renderer_->text_width(name, t.result_font) > avail) {
            while (name.size() > 1 && renderer_->text_width(name + "…", t.result_font) > avail) name.pop_back();
            name += "…";
        }
        bool has_snip = it.kind == ItemKind::Content && !it.snippet.empty();
        if (it.description.empty() && !has_snip) {
            renderer_->draw_text(tx, ry + (layout_.row_h - static_cast<int>(t.result_font.size)) / 2 - 2,
                                 name, t.result_font, Color::from_rgba(1, 1, 1, 1));
        } else {
            renderer_->draw_text(tx, ry + 7, name, t.result_font, Color::from_rgba(1, 1, 1, 1));
            int suby = ry + 7 + static_cast<int>(t.result_font.size) + 3;
            if (has_snip) {
                renderer_->draw_markup(tx, suby, snippet_markup(it.snippet, accent_hex),
                                       t.result_detail_font, t.text_muted, avail, 1);
            } else {
                std::string sub = it.description;
                if (renderer_->text_width(sub, t.result_detail_font) > avail) {
                    while (sub.size() > 1 && renderer_->text_width(sub + "…", t.result_detail_font) > avail) sub.pop_back();
                    sub += "…";
                }
                renderer_->draw_text(tx, suby, sub, t.result_detail_font, t.text_muted);
            }
        }
    };

    Color hdr = Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.65);

    // Section headers and rows come from the precomputed layout (relayout()).
    // Letter-spacing gives the uppercase category labels a refined, intentional
    // "Spotlight" tracking.
    RenderFontConfig hf = t.result_detail_font;
    hf.size = std::max(10.0, t.result_detail_font.size - 1);
    for (const auto& h : headers_)
        renderer_->draw_markup(row_x, py + h.y,
                               "<span letter_spacing=\"1400\">" + escape_markup(h.label) + "</span>",
                               hf, hdr);
    int total_rows = static_cast<int>(rows_.size());
    // Compute visible rows within the panel, for scrollbar.
    int vis_rows = 0;
    int list_top = py + (rows_.empty() ? 0 : rows_.front().y);
    for (const auto& r : rows_) {
        if (py + r.y + layout_.row_h <= py + panel_total_h_ - 20) vis_rows++;
    }
    if (vis_rows < total_rows && total_rows > 0) {
        int sb_x = px + layout_.list_w - 8;
        int sb_y = list_top;
        int sb_h = ph - (sb_y - py) - 20;
        int thumb_h = std::max(24, sb_h * vis_rows / total_rows);
        int thumb_y = sb_y + (scroll_offset_ * (sb_h - thumb_h)) / std::max(1, total_rows - vis_rows);
        renderer_->fill_rect(sb_x, sb_y, 4, sb_h,
                             Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.15));
        renderer_->rounded_rect(sb_x, thumb_y, 4, thumb_h, 2,
                                Color::from_rgba(t.accent.r, t.accent.g, t.accent.b, 0.6));
    }
    for (const auto& r : rows_)
        draw_row(items_[r.item_index], py + r.y, r.item_index == selected_index_,
                 r.hero ? layout_.icon_size + 6 : layout_.icon_size);

    // --- Preview pane (right column) ---
    render_preview(px, py, pw, ph, t);

    renderer_->end();
    wayland_->submit_buffer(buf, 0, 0);
    needs_redraw_ = false;
}

void LauncherUI::render_preview(int px, int py, int pw, int ph, const Theme& t) {
    int div_x = px + layout_.list_w;
    // vertical divider
    renderer_->fill_rect(div_x, py + layout_.search_h + 8, 1, ph - layout_.search_h - 16,
                         Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.35));

    if (items_.empty() || selected_index_ >= static_cast<int>(items_.size())) return;
    const ListItem& it = items_[selected_index_];

    int col_x = div_x + 1;
    int col_w = pw - layout_.list_w - 1;
    int inner_x = col_x + layout_.preview_pad;
    int inner_w = col_w - 2 * layout_.preview_pad;

    // Big icon, centred near the top.
    int isz = 76;
    renderer_->draw_icon(col_x + (col_w - isz) / 2, py + layout_.search_h + 26, isz,
                         it.icon_name, it.name, t.accent);

    int cy = py + layout_.search_h + 26 + isz + 18;

    auto draw_centered = [&](const std::string& s, const RenderFontConfig& f, const Color& c) {
        std::string text = s;
        if (renderer_->text_width(text, f) > inner_w) {
            while (text.size() > 1 && renderer_->text_width(text + "…", f) > inner_w) text.pop_back();
            text += "…";
        }
        int w = renderer_->text_width(text, f);
        renderer_->draw_text(col_x + (col_w - w) / 2, cy, text, f, c);
    };

    // Name (bold-ish) + a centered type badge pill.
    RenderFontConfig nf = t.result_font; nf.size = 15; nf.bold = true;
    draw_centered(it.name, nf, Color::from_rgba(1, 1, 1, 1));
    cy += static_cast<int>(nf.size) + 10;
    {
        std::string bl = kind_badge(it);
        RenderFontConfig bf = t.result_detail_font; bf.bold = true;
        bf.size = std::max(10.0, t.result_detail_font.size - 1);
        int tw = renderer_->text_width(bl, bf);
        int padx = 9, bh = static_cast<int>(bf.size) + 9, bw = tw + 2 * padx;
        int bx = col_x + (col_w - bw) / 2;
        renderer_->rounded_rect(bx, cy, bw, bh, bh / 2,
                                Color::from_rgba(t.accent.r, t.accent.g, t.accent.b, 0.16));
        renderer_->draw_text(bx + padx, cy + 4, bl, bf, t.accent);
        cy += bh + 16;
    }

    // Divider then key/value details.
    renderer_->fill_rect(inner_x, cy, inner_w, 1, Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.3));
    cy += 14;

    auto draw_kv = [&](const std::string& k, const std::string& v) {
        renderer_->draw_text(inner_x, cy, k, t.result_detail_font,
                             Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.9));
        std::string val = v;
        int vy = cy + static_cast<int>(t.result_detail_font.size) + 2;
        if (renderer_->text_width(val, t.result_detail_font) > inner_w) {
            while (val.size() > 1 && renderer_->text_width("…" + val, t.result_detail_font) > inner_w) val.erase(val.begin());
            val = "…" + val;
        }
        renderer_->draw_text(inner_x, vy, val, t.result_detail_font, Color::from_rgba(0.9, 0.9, 0.95, 1));
        cy = vy + static_cast<int>(t.result_detail_font.size) + 12;
    };

    if (it.kind == ItemKind::File || it.kind == ItemKind::Folder) {
        draw_kv("Where", abbreviate_home(std::filesystem::path(it.path).parent_path().string()));
        struct stat st;
        if (stat(it.path.c_str(), &st) == 0) {
            if (it.kind == ItemKind::File) draw_kv("Size", format_size(st.st_size));
            draw_kv("Modified", format_time(st.st_mtime));
        }
        renderer_->draw_text(inner_x, ph + py - 34, "⏎ Open   ·   right-click: reveal",
                             t.result_detail_font, t.text_muted);
    } else if (it.kind == ItemKind::Application) {
        if (!it.description.empty()) draw_kv("About", it.description);
        draw_kv("Command", it.path);
        renderer_->draw_text(inner_x, ph + py - 34, "⏎ Launch   ·   right-click: reveal",
                             t.result_detail_font, t.text_muted);
    } else if (it.kind == ItemKind::Calculator) {
        draw_kv("Expression", it.description);
        draw_kv("Result", it.path);
        renderer_->draw_text(inner_x, ph + py - 34, "⏎ Copy result", t.result_detail_font, t.text_muted);
    } else if (it.kind == ItemKind::Content) {
        draw_kv("Where", abbreviate_home(std::filesystem::path(it.path).parent_path().string()));
        struct stat st;
        bool have = stat(it.path.c_str(), &st) == 0;
        if (have) draw_kv("Size", format_size(st.st_size));
        if (have) draw_kv("Modified", format_time(st.st_mtime));
        if (!it.snippet.empty()) {
            renderer_->draw_text(inner_x, cy, "Match", t.result_detail_font,
                                 Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.9));
            cy += static_cast<int>(t.result_detail_font.size) + 6;
            // Native word-wrap + inline highlight of the matched runs (up to 6 lines).
            int h = renderer_->draw_markup(inner_x, cy, snippet_markup(it.snippet, color_hex(t.accent)),
                                           t.result_detail_font, Color::from_rgba(0.9, 0.9, 0.95, 1),
                                           inner_w, 6);
            cy += h + 8;
        }
        renderer_->draw_text(inner_x, ph + py - 34, "⏎ Open   ·   right-click: reveal",
                             t.result_detail_font, t.text_muted);
    }
}

} // namespace waylaunch
