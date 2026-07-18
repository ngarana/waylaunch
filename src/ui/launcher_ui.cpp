#include "waylaunch/launcher_ui.h"
#include "waylaunch/wayland_core.h"
#include "waylaunch/renderer.h"
#include "waylaunch/config.h"
#include "waylaunch/clipboard.h"
#include "waylaunch/calculator.h"
#include "waylaunch/app_launcher.h"
#include "waylaunch/subprocess.h"
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>

namespace waylaunch {

namespace {

bool ui_dbg() { static bool v = std::getenv("WAYLAUNCH_DEBUG") != nullptr; return v; }


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
LauncherUI::~LauncherUI() = default;

bool LauncherUI::init(Config& config) {
    config_ = &config;

    wayland_ = std::make_unique<WaylandCore>();
    if (!wayland_->init()) return false;

    renderer_ = std::make_unique<Renderer>();

    search_ = std::make_unique<SearchManager>();
    search_->set_max_results(config.get().search.max_results);
    search_->set_debounce_ms(config.get().search.debounce_ms);

    if (!config.get().search.paths.empty()) {
        search_->set_search_path(config.get().search.paths[0].path);
    }

    wayland_->set_key_handler([this](uint32_t k, uint32_t u, bool p) { on_key(k, u, p); });
    wayland_->set_mouse_handler([this](double x, double y, uint32_t b, bool p) { on_mouse(x, y, b, p); });
    wayland_->set_axis_handler([this](double x, double y, int32_t a, double v) { on_axis(x, y, a, v); });
    wayland_->set_close_handler([this]() { on_close(); });
    wayland_->set_redraw_handler([this]() { on_redraw(); });

    refresh_applications();
    return true;
}

void LauncherUI::run() {
    // Drive Wayland ourselves so that *every* event batch is followed by a
    // repaint when the UI is dirty. Previously rendering was scattered across
    // individual handlers, so key presses that only changed the query (e.g.
    // typing in Applications mode) updated state but never repainted — the
    // overlay looked frozen while holding an exclusive keyboard grab.
    // The first paint is triggered by the layer-surface configure event
    // (on_redraw), which is the earliest point a buffer may legally be committed.
    wayland_->set_running(true);
    while (wayland_->is_running()) {
        if (wayland_->dispatch() < 0) break;
        if (needs_redraw_) render_frame();
    }
}

void LauncherUI::quit() {
    wayland_->quit();
}

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
    t.input_font = RenderFontConfig{tc.input_font.family, tc.input_font.size};
    t.result_font = RenderFontConfig{tc.result_font.family, tc.result_font.size};
    t.result_detail_font = RenderFontConfig{tc.result_detail_font.family, tc.result_detail_font.size};
    return t;
}

// ---------------------------------------------------------------------------
// Modes / search
// ---------------------------------------------------------------------------

void LauncherUI::set_mode(LauncherModeType mode) {
    current_mode_ = mode;
    query_.clear();
    cursor_pos_ = 0;
    selected_index_ = 0;
    scroll_offset_ = 0;
    items_.clear();
    needs_redraw_ = true;

    if (mode == LauncherModeType::Applications) refresh_applications();
    else if (mode != LauncherModeType::Calculator) update_search();
}

void LauncherUI::refresh_applications() {
    AppLauncher app;
    std::vector<std::string> paths;
    for (auto& p : config_->get().search.paths) {
        if (p.type == "desktop") paths.push_back(p.path);
    }
    app.set_search_paths(paths);
    app.scan();
    auto entries = app.search(query_);
    items_.clear();
    for (auto& e : entries) {
        ListItem li;
        li.name = e.name;
        li.path = e.exec;
        li.description = e.comment;
        li.icon_name = e.icon;
        items_.push_back(std::move(li));
    }
    selected_index_ = 0;
    scroll_offset_ = 0;
    needs_redraw_ = true;
}

void LauncherUI::toggle_mode() {
    int m = (static_cast<int>(current_mode_) + 1) % 4;
    set_mode(static_cast<LauncherModeType>(m));
}

void LauncherUI::update_search() {
    if (current_mode_ == LauncherModeType::Calculator) {
        items_.clear();
        needs_redraw_ = true;
        return;
    }
    if (current_mode_ == LauncherModeType::Applications) {
        refresh_applications();
        return;
    }

    SearchMode mode = (current_mode_ == LauncherModeType::FileContents) ? SearchMode::FileContents : SearchMode::Files;
    search_->search(query_, mode, [this](std::vector<SearchResult> results) {
        update_items(results);
    });
}

void LauncherUI::update_items(const std::vector<SearchResult>& results) {
    items_.clear();
    for (const auto& r : results) {
        ListItem item;
        item.name = r.display_name;
        item.path = r.path;
        item.description = r.display_path.empty() ? r.description : r.display_path;
        item.icon_name = r.icon_name;
        item.score = r.score;
        items_.push_back(std::move(item));
    }
    selected_index_ = 0;
    scroll_offset_ = 0;
    needs_redraw_ = true;
    render_frame();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

int LauncherUI::visible_row_count() const {
    return std::min(static_cast<int>(items_.size()) - scroll_offset_, layout_.max_rows);
}

int LauncherUI::panel_height() const {
    if (items_.empty() && current_mode_ != LauncherModeType::Calculator) {
        return layout_.search_h + layout_.row_h;  // search + empty hint
    }
    int rows = static_cast<int>(items_.size());
    if (current_mode_ == LauncherModeType::Calculator) rows = 1;
    int shown = std::min(rows, layout_.max_rows + 1);  // +1 for the top-hit hero
    int h = layout_.search_h;
    h += layout_.section_pad;                 // TOP HIT header
    h += layout_.row_h + layout_.section_pad; // hero
    if (shown > 1) {
        h += layout_.section_pad;             // group header
        h += (shown - 1) * layout_.row_h;
    }
    h += layout_.pad_x;
    return h;
}

void LauncherUI::select_item(int index) {
    if (items_.empty()) return;
    int max = static_cast<int>(items_.size()) - 1;
    selected_index_ = std::max(0, std::min(index, max));
    if (selected_index_ < scroll_offset_) scroll_offset_ = selected_index_;
    else if (selected_index_ >= scroll_offset_ + visible_row_count()) {
        scroll_offset_ = selected_index_ - visible_row_count() + 1;
    }
    needs_redraw_ = true;
    render_frame();
}

void LauncherUI::autocomplete() {
    if (items_.empty()) return;
    query_ = items_[selected_index_].name;
    cursor_pos_ = query_.size();
    update_search();
}

void LauncherUI::launch_selected() {
    if (current_mode_ == LauncherModeType::Calculator) {
        Calculator calc;
        auto result = calc.evaluate(query_);
        if (result.valid) { Clipboard::copy_text(result.result); quit(); }
        return;
    }
    if (items_.empty() || selected_index_ >= static_cast<int>(items_.size())) return;
    const auto& item = items_[selected_index_];
    if (ui_dbg()) fprintf(stderr, "[ui] launch mode=%d name='%s' path='%s'\n",
                          static_cast<int>(current_mode_), item.name.c_str(), item.path.c_str());

    // Custom command (future [[commands]] entries) takes precedence.
    if (!item.action_command.empty()) {
        spawn_detached({"/bin/sh", "-c", item.action_command});
    } else if (current_mode_ == LauncherModeType::Applications) {
        // For applications, item.path holds the .desktop Exec line (field codes
        // already stripped). It is a command to run, NOT a file — execute it via
        // a shell so arguments and env prefixes work. Using xdg-open here is what
        // produced "xdg-open: file 'alacritty' does not exist".
        if (!item.path.empty()) spawn_detached({"/bin/sh", "-c", item.path});
    } else if (!item.path.empty()) {
        // Files / FileContents: item.path is a real filesystem path — open it
        // with the user's default handler.
        spawn_detached({"xdg-open", item.path});
    }
    quit();
}

void LauncherUI::open_file_location(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;

    // Only file-backed results have a real location to reveal. Applications
    // store an exec command in `path`, and the calculator has no path — for
    // those, right-click is a no-op ("where necessary").
    if (current_mode_ != LauncherModeType::Files &&
        current_mode_ != LauncherModeType::FileContents) {
        return;
    }

    const std::string& raw = items_[index].path;
    if (raw.empty()) return;

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(raw, ec);
    if (ec) abs = raw;
    if (!std::filesystem::exists(abs, ec)) return;

    // Preferred: the freedesktop FileManager1 D-Bus interface, which opens the
    // enclosing folder AND selects the file (Nautilus, Dolphin, Nemo, PCManFM,
    // Thunar, …). This is the faithful "reveal in Finder" behaviour.
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

    // Fallback: open the enclosing directory with the default handler. The file
    // itself won't be pre-selected, but its location is shown.
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
    if (!pressed) return;

    bool ctrl = wayland_->kbd_.state && xkb_state_mod_name_is_active(
        wayland_->kbd_.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);

    if (ctrl) {
        if (keysym == XKB_KEY_j || keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_k || keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
        if (keysym == XKB_KEY_n) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_p) { select_item(selected_index_ - 1); return; }
    }

    if (keysym == XKB_KEY_Return) { launch_selected(); return; }
    if (keysym == XKB_KEY_Escape) { quit(); return; }
    if (keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
    if (keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
    if (keysym == XKB_KEY_Tab) { autocomplete(); return; }
    if (keysym == XKB_KEY_Page_Up) { select_item(selected_index_ - layout_.max_rows); return; }
    if (keysym == XKB_KEY_Page_Down) { select_item(selected_index_ + layout_.max_rows); return; }
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
    } else if (keysym == XKB_KEY_Right && cursor_pos_ < query_.size()) {
        while (cursor_pos_ < query_.size() && (query_[cursor_pos_] & 0xC0) == 0x80) cursor_pos_++;
    }

    needs_redraw_ = true;
}

void LauncherUI::on_mouse(double x, double y, uint32_t button, bool pressed) {
    if (ui_dbg()) fprintf(stderr, "[ui] on_mouse x=%.0f y=%.0f btn=0x%x pressed=%d\n", x, y, button, pressed);
    if (!pressed) return;

    constexpr uint32_t BTN_LEFT = 0x110;
    constexpr uint32_t BTN_RIGHT = 0x111;

    // Click anywhere outside the panel dismisses the launcher (Spotlight-style).
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
        // Right-click reveals the item's location in the file manager.
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
    int ph = panel_height();
    int pw = layout_.win_w;
    int px = (wayland_->surface_width() - pw) / 2;
    int py = layout_.margin_top;

    if (x < px || x > px + pw || y < py || y > py + ph) return -1;
    int ly = static_cast<int>(y) - py;

    if (ly <= layout_.search_h) return -1;  // search field itself

    int yy = layout_.search_h + layout_.section_pad;
    // top hit hero
    if (ly >= yy && ly < yy + layout_.row_h) {
        return scroll_offset_ + 0;
    }
    yy += layout_.row_h + layout_.section_pad;
    yy += layout_.section_pad;  // group header
    int idx = static_cast<int>((ly - yy) / layout_.row_h) + scroll_offset_ + 1;
    if (idx >= scroll_offset_ + 1 && idx < scroll_offset_ + 1 + visible_row_count()) return idx;
    return -1;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void LauncherUI::render_frame() {
    if (!needs_redraw_) return;

    Buffer* buf = wayland_->acquire_buffer();
    if (!buf) return;

    Theme t = build_theme();
    int bw = buf->width;
    int bh = buf->height;

    renderer_->begin(buf->data, buf->stride, bw, bh);

    // Clear the whole buffer transparent; the panel is drawn on top.
    renderer_->clear(Color::from_rgba(0, 0, 0, 0));

    int ph = panel_height();
    int pw = layout_.win_w;
    int px = (bw - pw) / 2;   // centre within the surface we actually draw into
    int py = layout_.margin_top;

    // Drop shadow + panel background (translucent like Spotlight's blurred glass).
    renderer_->rounded_rect(px - 2, py + 4, pw + 4, ph + 4, layout_.corner_radius + 2,
                            Color::from_rgba(0, 0, 0, 0.35));
    Color panel = t.background;
    renderer_->rounded_rect(px, py, pw, ph, layout_.corner_radius,
                            Color::from_rgba(panel.r, panel.g, panel.b, 0.92));

    // Hairline border
    renderer_->fill_rect(px, py, pw, 1, Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.6));
    renderer_->fill_rect(px, py + ph - 1, pw, 1, Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.6));

    // --- Search field ---
    int glyph_size = 26;
    int gx = px + layout_.search_pad_x;
    int gy = py + (layout_.search_h - glyph_size) / 2;
    renderer_->draw_search_glyph(gx, gy, glyph_size, Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.9));

    int text_x = gx + glyph_size + 14;
    RenderFontConfig sf = t.input_font;
    sf.size = 22;
    if (query_.empty()) {
        renderer_->draw_text(text_x, py + (layout_.search_h - static_cast<int>(sf.size)) / 2,
                             config_->get().search.placeholder, sf,
                             Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.8));
    } else {
        renderer_->draw_text(text_x, py + (layout_.search_h - static_cast<int>(sf.size)) / 2,
                             query_, sf, Color::from_rgba(1, 1, 1, 1));
        // caret
        if (cursor_pos_ < query_.size()) {
            int caret_x = text_x + renderer_->text_width(query_.substr(0, cursor_pos_), sf);
            renderer_->fill_rect(caret_x + 1, py + 14, 2, layout_.search_h - 28, t.accent);
        }
    }

    // divider under search
    renderer_->fill_rect(px, py + layout_.search_h, pw, 1, Color::from_rgba(t.border.r, t.border.g, t.border.b, 0.35));

    // --- Calculator mode ---
    if (current_mode_ == LauncherModeType::Calculator) {
        Calculator calc;
        auto result = calc.evaluate(query_);
        int cy = py + layout_.search_h + layout_.section_pad;
        renderer_->draw_icon(px + layout_.pad_x, cy + (layout_.row_h - layout_.icon_size) / 2,
                             layout_.icon_size, "accessories-calculator", "Calc", t.accent);
        int tx = px + layout_.pad_x + layout_.icon_size + layout_.icon_pad;
        RenderFontConfig cf = t.result_font;
        cf.size = 20; cf.bold = true;
        if (query_.empty()) {
            renderer_->draw_text(tx, cy + (layout_.row_h - 20) / 2, "Type a calculation…",
                                 t.result_detail_font, t.text_muted);
        } else if (result.valid) {
            renderer_->draw_text(tx, cy + (layout_.row_h - 20) / 2, "= " + result.result,
                                 cf, Color::from_rgba(1, 1, 1, 1));
            // action hint on the right
            renderer_->draw_text(px + pw - layout_.pad_x - 180, cy + (layout_.row_h - 14) / 2,
                                 "⏎ Copy result", t.result_detail_font, t.text_muted);
        } else {
            renderer_->draw_text(tx, cy + (layout_.row_h - 20) / 2, "Invalid expression",
                                 t.result_detail_font, t.error);
        }
        renderer_->end();
        wayland_->submit_buffer(buf, 0, 0);
        needs_redraw_ = false;
        return;
    }

    // --- Empty state ---
    if (items_.empty()) {
        int ey = py + layout_.search_h + layout_.row_h / 2 - 8;
        renderer_->draw_text(px + layout_.pad_x, ey, "No results", t.result_font, t.text_muted);
        renderer_->end();
        wayland_->submit_buffer(buf, 0, 0);
        needs_redraw_ = false;
        return;
    }

    int yy = py + layout_.search_h + layout_.section_pad;

    // Section header: TOP HIT
    renderer_->draw_text(px + layout_.pad_x, yy, "TOP HIT",
                         t.result_detail_font, Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.7));
    yy += layout_.section_pad;

    // Hero (index 0)
    {
        const auto& it = items_[scroll_offset_ + 0 < static_cast<int>(items_.size()) ? scroll_offset_ + 0 : 0];
        int row = scroll_offset_;
        bool sel = (row == selected_index_);
        int rx = px + layout_.pad_x;
        int ry = yy;
        int ic = layout_.icon_size + 6;
        if (sel) renderer_->rounded_rect(rx - 6, ry - 2, pw - 2 * layout_.pad_x + 12, layout_.row_h, 8,
                                         Color::from_rgba(t.selection.r, t.selection.g, t.selection.b, 0.55));
        renderer_->draw_icon(rx, ry + (layout_.row_h - ic) / 2, ic, it.icon_name, it.name, t.accent);
        int tx = rx + ic + layout_.icon_pad;
        renderer_->draw_text(tx, ry + 6, it.name, t.result_font, Color::from_rgba(1, 1, 1, 1));
        if (!it.description.empty())
            renderer_->draw_text(tx, ry + 6 + static_cast<int>(t.result_font.size) + 2,
                                 it.description, t.result_detail_font, t.text_muted);
    }
    yy += layout_.row_h + layout_.section_pad;

    // Group header
    const char* group = (current_mode_ == LauncherModeType::Applications) ? "APPLICATIONS"
                      : (current_mode_ == LauncherModeType::FileContents) ? "CONTENT"
                      : "FILES";
    renderer_->draw_text(px + layout_.pad_x, yy, group,
                         t.result_detail_font, Color::from_rgba(t.text_muted.r, t.text_muted.g, t.text_muted.b, 0.7));
    yy += layout_.section_pad;

    int count = visible_row_count();
    for (int i = 0; i < count; i++) {
        int row = scroll_offset_ + 1 + i;
        if (row >= static_cast<int>(items_.size())) break;
        const auto& it = items_[row];
        bool sel = (row == selected_index_);
        int rx = px + layout_.pad_x;
        int ry = yy + i * layout_.row_h;
        if (sel) renderer_->rounded_rect(rx - 6, ry - 2, pw - 2 * layout_.pad_x + 12, layout_.row_h, 8,
                                         Color::from_rgba(t.selection.r, t.selection.g, t.selection.b, 0.55));
        renderer_->draw_icon(rx, ry + (layout_.row_h - layout_.icon_size) / 2, layout_.icon_size,
                             it.icon_name, it.name, t.accent);
        int tx = rx + layout_.icon_size + layout_.icon_pad;
        renderer_->draw_text(tx, ry + (layout_.row_h - static_cast<int>(t.result_font.size)) / 2 - 6,
                             it.name, t.result_font, Color::from_rgba(1, 1, 1, 1));
        if (!it.description.empty())
            renderer_->draw_text(tx, ry + (layout_.row_h - static_cast<int>(t.result_font.size)) / 2 + 10,
                                 it.description, t.result_detail_font, t.text_muted);
    }

    // Scrollbar if overflowing
    int total = static_cast<int>(items_.size());
    if (total > layout_.max_rows + 1) {
        int track_y = py + layout_.search_h + 8;
        int track_h = ph - layout_.search_h - 16;
        int thumb_h = std::max(24, track_h * (layout_.max_rows + 1) / total);
        int thumb_y = track_y + (scroll_offset_ * (track_h - thumb_h)) / std::max(1, total - (layout_.max_rows + 1));
        renderer_->rounded_rect(px + pw - 6, thumb_y, 4, thumb_h, 2,
                                Color::from_rgba(t.accent.r, t.accent.g, t.accent.b, 0.7));
    }

    renderer_->end();
    wayland_->submit_buffer(buf, 0, 0);
    needs_redraw_ = false;
}

} // namespace waylaunch
