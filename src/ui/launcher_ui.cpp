#include "waylaunch/launcher_ui.h"
#include "waylaunch/wayland_core.h"
#include "waylaunch/renderer.h"
#include "waylaunch/search_manager.h"
#include "waylaunch/config.h"
#include "waylaunch/clipboard.h"
#include "waylaunch/calculator.h"
#include "waylaunch/app_launcher.h"
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace waylaunch {

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

    wayland_->set_key_handler([this](uint32_t keysym, uint32_t utf32, bool pressed) {
        on_key(keysym, utf32, pressed);
    });
    wayland_->set_mouse_handler([this](double x, double y, uint32_t button, bool pressed) {
        on_mouse(x, y, button, pressed);
    });
    wayland_->set_axis_handler([this](double x, double y, int32_t axis, double value) {
        on_axis(x, y, axis, value);
    });
    wayland_->set_close_handler([this]() { on_close(); });
    wayland_->set_redraw_handler([this]() { on_redraw(); });

    set_mode(LauncherModeType::Applications);
    return true;
}

void LauncherUI::run() {
    wayland_->run();
}

void LauncherUI::quit() {
    wayland_->quit();
}

void LauncherUI::set_mode(LauncherModeType mode) {
    current_mode_ = mode;
    query_.clear();
    cursor_pos_ = 0;
    selected_index_ = 0;
    scroll_offset_ = 0;
    items_.clear();
    needs_redraw_ = true;

    switch (mode) {
        case LauncherModeType::Applications: {
            AppLauncher app;
            app.set_search_paths(config_->get().search.paths.empty()
                ? std::vector<std::string>{}
                : [&]() {
                    std::vector<std::string> paths;
                    for (auto& p : config_->get().search.paths) {
                        if (p.type == "desktop") paths.push_back(p.path);
                    }
                    return paths;
                }());
            app.scan();
            auto entries = app.search("");
            items_.clear();
            for (auto& e : entries) {
                ListItem li;
                li.name = e.name;
                li.path = e.exec;
                li.description = e.comment;
                li.icon_name = e.icon;
                items_.push_back(std::move(li));
            }
            break;
        }
        case LauncherModeType::Files:
        case LauncherModeType::FileContents:
            update_search();
            break;
        case LauncherModeType::Calculator:
            break;
    }
}

void LauncherUI::toggle_mode() {
    int m = (static_cast<int>(current_mode_) + 1) % 4;
    set_mode(static_cast<LauncherModeType>(m));
}

void LauncherUI::on_key(uint32_t keysym, uint32_t utf32, bool pressed) {
    if (!pressed) return;

    // Handle Ctrl+J/K for vim-style navigation
    bool ctrl = wayland_->kbd_.state && xkb_state_mod_name_is_active(
        wayland_->kbd_.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE);

    if (ctrl) {
        if (keysym == XKB_KEY_j || keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_k || keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
        if (keysym == XKB_KEY_n) { select_item(selected_index_ + 1); return; }
        if (keysym == XKB_KEY_p) { select_item(selected_index_ - 1); return; }
    }

    // Direct key bindings
    if (keysym == XKB_KEY_Return) {
        if (current_mode_ == LauncherModeType::Calculator) {
            Calculator calc;
            auto result = calc.evaluate(query_);
            if (result.valid) { Clipboard::copy_text(result.result); quit(); }
        } else {
            launch_selected();
        }
        return;
    }
    if (keysym == XKB_KEY_Escape) { quit(); return; }
    if (keysym == XKB_KEY_Up) { select_item(selected_index_ - 1); return; }
    if (keysym == XKB_KEY_Down) { select_item(selected_index_ + 1); return; }
    if (keysym == XKB_KEY_Tab) { toggle_mode(); return; }
    if (keysym == XKB_KEY_Page_Up) { select_item(selected_index_ - 10); return; }
    if (keysym == XKB_KEY_Page_Down) { select_item(selected_index_ + 10); return; }
    if (keysym == XKB_KEY_Home) { select_item(0); return; }
    if (keysym == XKB_KEY_End) { select_item(static_cast<int>(items_.size()) - 1); return; }

    // Text input
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
    if (!pressed) return;

    // BTN_LEFT = 0x110
    if (button == 0x110) {
        int idx = hit_test(x, y);
        if (idx >= 0 && idx < static_cast<int>(items_.size())) {
            if (idx == selected_index_) {
                launch_selected();
            } else {
                select_item(idx);
            }
        }
    }
    // BTN_RIGHT = 0x111 - could open actions menu in the future
}

void LauncherUI::on_axis(double, double, int32_t axis, double value) {
    // axis 0 = vertical scroll, value > 0 = scroll down
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

void LauncherUI::update_search() {
    if (current_mode_ == LauncherModeType::Calculator) {
        // Evaluate as expression and show result preview
        Calculator calc;
        auto result = calc.evaluate(query_);
        items_.clear();
        if (result.valid) {
            ListItem li;
            li.name = "= " + result.result;
            li.path = result.result;
            items_.push_back(std::move(li));
        }
        selected_index_ = 0;
        scroll_offset_ = 0;
        needs_redraw_ = true;
        return;
    }

    if (current_mode_ == LauncherModeType::Applications) {
        // Re-scan and filter
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
        item.description = r.snippet;
        item.icon_name = r.icon_name;
        item.score = r.score;
        items_.push_back(std::move(item));
    }
    selected_index_ = 0;
    scroll_offset_ = 0;
    needs_redraw_ = true;
    // Trigger a redraw from the UI thread
    render_frame();
}

void LauncherUI::select_item(int index) {
    if (items_.empty()) return;
    int max = static_cast<int>(items_.size()) - 1;
    selected_index_ = std::max(0, std::min(index, max));
    const LayoutMetrics layout{};
    if (selected_index_ < scroll_offset_) scroll_offset_ = selected_index_;
    else if (selected_index_ >= scroll_offset_ + layout.max_visible_items) {
        scroll_offset_ = selected_index_ - layout.max_visible_items + 1;
    }
    needs_redraw_ = true;
    render_frame();
}

void LauncherUI::launch_selected() {
    if (items_.empty() || selected_index_ >= static_cast<int>(items_.size())) return;
    const auto& item = items_[selected_index_];
    if (!item.action_command.empty()) {
        std::string cmd = item.action_command + " &";
        system(cmd.c_str());
    } else if (!item.path.empty()) {
        std::string cmd = "xdg-open \"" + item.path + "\" &";
        system(cmd.c_str());
    }
    quit();
}

int LauncherUI::hit_test(double x, double y) const {
    const LayoutMetrics layout{};
    int items_y = layout.padding + layout.input_height + layout.padding;
    if (y < items_y || y > items_y + layout.max_visible_items * layout.item_height) return -1;
    int idx = static_cast<int>((y - items_y) / layout.item_height) + scroll_offset_;
    if (idx >= 0 && idx < static_cast<int>(items_.size())) return idx;
    return -1;
}

void LauncherUI::render_frame() {
    if (!needs_redraw_) return;

    Buffer* buf = wayland_->acquire_buffer();
    if (!buf) return;

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

    LayoutMetrics layout;
    std::vector<RenderItem> render_items;
    for (size_t i = 0; i < items_.size(); i++) {
        RenderItem ri;
        ri.segments.push_back({items_[i].name, t.foreground});
        if (!items_[i].description.empty()) {
            ri.segments.push_back({" - " + items_[i].description, t.text_muted});
        }
        ri.selected = (static_cast<int>(i) == selected_index_);
        ri.icon_name = items_[i].icon_name;
        render_items.push_back(std::move(ri));
    }

    renderer_->render_into(
        buf->data, buf->stride, buf->width, buf->height, wayland_->primary_scale(),
        t, layout, query_, cursor_pos_, render_items, scroll_offset_, selected_index_);

    wayland_->submit_buffer(buf, 0, 0);
    needs_redraw_ = false;
}

} // namespace waylaunch
