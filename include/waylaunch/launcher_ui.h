#pragma once

#include "waylaunch/history.h"
#include "waylaunch/list_item.h"
#include "waylaunch/renderer.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace waylaunch {

class WaylandCore;
class Renderer;
class Config;
class AppLauncher;
class WlrForeignToplevelBackend;
class AppSwitcherManager;
class SwitcherInputController;
class SwitcherRenderer;
class PowerActionBackend;
class PowerManager;
class PowerInputController;
class PowerRenderer;
class ResultProvider;
namespace content {
class Store;
}

// macOS Spotlight visual geometry (two-column: result list + preview pane).
struct SpotlightLayout {
    int win_w = 720; // floating panel width
    int corner_radius = 16;
    int margin_top = 150; // Spotlight sits in the upper third, not pinned to the very top
    int search_h = 64;    // large search field
    int search_pad_x = 22;
    int row_h = 56;
    int icon_size = 34;
    int icon_pad = 14;
    int pad_x = 14;
    int max_per_group = 6; // cap results shown per category (Spotlight-style)
    int section_pad = 12;
    int header_h = 24; // category header row height
    int border = 1;
    int list_w = 402;     // width of the left result column (within win_w)
    int preview_pad = 22; // inner padding of the preview pane
};

class LauncherUI {
  public:
    LauncherUI();
    ~LauncherUI();

    bool init(Config& config);
    void run();
    void quit();

    void set_initial_query(std::string q) { initial_query_ = std::move(q); }
    void set_config_path(std::string p) { config_path_ = std::move(p); }
    // Start straight into the app switcher (Alt+Tab entry) instead of search:
    // show the switcher at startup and exit when a selection is confirmed.
    void set_switcher_mode(bool v) { switcher_mode_ = v; }
    // Preselect the far end of the list (Alt+Shift+Tab / reverse cycle).
    void set_switcher_reverse(bool v) { switcher_reverse_ = v; }
    // Start as the one-shot power-actions overlay (waylaunch --power).
    void set_power_mode(bool v) { power_mode_ = v; }

  private:
    // Precomputed layout of the result list (headers + rows), so panel_height(),
    // render, and hit-testing all agree without duplicating layout math.
    struct RowSlot {
        int item_index;
        int y;
        bool hero;
    };
    struct HeaderSlot {
        std::string label;
        int y;
    };
    // --- Wayland event callbacks ---
    void on_key(uint32_t keysym, uint32_t utf32, bool pressed);
    void on_mouse(double x, double y, uint32_t button, bool pressed);
    void on_mouse_move(double x, double y);
    void on_axis(double x, double y, int32_t axis, double value);
    void on_close();
    void on_redraw();

    // --- Unified search ---
    void scan_apps();          // scan .desktop files once at startup
    void register_providers(); // build the enabled ResultProvider list (§5.2)
    void update_search();      // query changed: rebuild sync results, kick async files
    void rebuild_app_items();  // apps + calculator for the current query (synchronous)
    void rebuild_items();      // merge app_items_ + file_items_ into items_
    void kick_file_search();   // hand the current query to the worker thread
    void apply_file_results(); // main-thread: take results the worker posted

    void select_item(int index);
    void launch_selected(); // activate selected item by kind
    void open_file_location(int index);
    void autocomplete();

    // --- Rendering ---
    void relayout(); // recompute header/row slots + panel height
    void render_frame();
    void render_preview(int px, int py, int pw, int ph, const Theme& t);
    // A recent-search result (ItemKind::History) has no rich preview of its own.
    // If its query resolves to an application or an existing file/folder, fill
    // `out` with that target so the preview shows it instead of a bare
    // "Query / Usage" card. Synchronous (apps in-memory; a literal path stat()'d).
    bool resolve_recent_preview(const std::string& query, ListItem& out);
    int hit_test(double x, double y) const;
    int panel_height() const;

    Theme build_theme() const;

    // --- File-search worker ---
    void file_worker_loop();

    std::unique_ptr<WaylandCore> wayland_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<AppLauncher> apps_;
    Config* config_ = nullptr;
    HistoryStore history_;

    // Pluggable search sources (§5.2). Being migrated incrementally; providers
    // already registered here own their query + activation, the rest still runs
    // through the inline pipeline below.
    std::vector<std::unique_ptr<ResultProvider>> providers_;

    std::string query_;
    size_t cursor_pos_ = 0;
    int selected_index_ = 0;
    int scroll_offset_ = 0;

    std::vector<ListItem> items_;         // combined list actually rendered
    std::vector<ListItem> app_items_;     // synchronous: calculator + applications
    std::vector<ListItem> file_items_;    // asynchronous: files/folders from the worker
    std::vector<ListItem> content_items_; // asynchronous: content matches (CONTENTS section)

    std::vector<RowSlot> rows_;       // computed row layout (relative to panel top)
    std::vector<HeaderSlot> headers_; // computed section headers
    int panel_total_h_ = 0;

    std::string initial_query_;

    SpotlightLayout layout_;
    bool needs_redraw_ = true;
    bool blur_enabled_ = false; // compositor backdrop blur (Hyprland) → glass panel

    // SIGINT/SIGTERM → the run() poll loop (the overlay grabs the keyboard, so
    // it must exit cleanly on a kill signal, not just on Esc).
    int signal_fd_ = -1;

    // Async file search (worker thread → main loop via eventfd).
    int results_fd_ = -1;
    std::thread file_thread_;
    std::mutex file_mtx_;
    std::condition_variable file_cv_;
    std::string file_pending_query_;
    bool file_has_pending_ = false;
    bool file_stop_ = false;
    uint64_t file_gen_ = 0;            // generation counter; stale results are dropped
    std::vector<ListItem> file_ready_; // results the worker finished, awaiting the main thread
    uint64_t file_ready_gen_ = 0;
    std::vector<std::string> file_roots_;
    std::vector<std::string> file_excludes_;
    int file_min_query_ = 2;
    int max_file_results_ = 6;
    bool file_enabled_ = true;

    // Content search (read-only queries against the waylaunchd index).
    std::string config_path_;                       // to load [content] settings
    std::unique_ptr<content::Store> content_store_; // read-only; null if unavailable
    bool content_enabled_ = false;
    int content_min_query_ = 3;
    int content_max_results_ = 6;
    std::vector<ListItem> content_ready_; // worker → main (guarded by file_mtx_)

    // App Switcher (Command+Tab / Alt+Tab)
    std::unique_ptr<WlrForeignToplevelBackend> switcher_backend_;
    std::unique_ptr<AppSwitcherManager> switcher_manager_;
    std::unique_ptr<SwitcherInputController> switcher_input_;
    std::unique_ptr<SwitcherRenderer> switcher_renderer_;
    bool switcher_mode_ = false;       // launched as the dedicated Alt+Tab overlay
    bool switcher_reverse_ = false;    // preselect the far end (Alt+Shift+Tab)
    bool switcher_shown_ = false;      // switcher currently mapped (visible this cycle)
    int switcher_advance_fd_ = -1;     // SIGUSR1 (re-invocation) → show/advance forward
    int switcher_reverse_fd_ = -1;     // SIGUSR2 (re-invocation) → show/step reverse
    bool switcher_go_dormant_ = false; // hidden this frame → unmap AFTER activate() lands

    // Power overlay (waylaunch --power): one-shot switcher-style HUD. All power
    // logic lives in src/power/; this class only creates, routes, and renders.
    std::unique_ptr<PowerActionBackend> power_backend_;
    std::unique_ptr<PowerManager> power_manager_;
    std::unique_ptr<PowerInputController> power_input_;
    std::unique_ptr<PowerRenderer> power_renderer_;
    bool power_mode_ = false;
    int power_last_remaining_ = -1; // last countdown second painted
};

} // namespace waylaunch
