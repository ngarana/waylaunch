#include "waylaunch/toolbar.h"
#include <algorithm>
#include <sstream>
#include <filesystem>

namespace waylaunch {

Toolbar::Toolbar() = default;
Toolbar::~Toolbar() = default;

void Toolbar::set_height(int height) {
    height_ = height;
}

int Toolbar::height() const {
    return height_;
}

void Toolbar::set_current_path(const std::string& path) {
    if (current_path_ != path) {
        current_path_ = path;
        update_breadcrumbs();
    }
}

const std::string& Toolbar::current_path() const {
    return current_path_;
}

void Toolbar::set_view_mode(ViewMode mode) {
    view_mode_ = mode;
}

void Toolbar::set_search_text(const std::string& text) {
    search_text_ = text;
}

void Toolbar::set_search_placeholder(const std::string& placeholder) {
    search_placeholder_ = placeholder;
}

void Toolbar::set_back_enabled(bool enabled) {
    if (!buttons_.empty()) buttons_[0].enabled = enabled;
}

void Toolbar::set_forward_enabled(bool enabled) {
    if (buttons_.size() > 1) buttons_[1].enabled = enabled;
}

void Toolbar::set_up_enabled(bool enabled) {
    if (buttons_.size() > 2) buttons_[2].enabled = enabled;
}

void Toolbar::set_search_scopes(const std::vector<SearchScope>& scopes) {
    scopes_ = scopes;
}

void Toolbar::set_active_scope(int index) {
    if (index >= 0 && index < static_cast<int>(scopes_.size())) {
        active_scope_ = index;
    }
}

int Toolbar::active_scope() const {
    return active_scope_;
}

int Toolbar::hit_test(double x, double y) const {
    if (y < 0 || y >= height_) return -1;

    for (size_t i = 0; i < buttons_.size(); i++) {
        const auto& btn = buttons_[i];
        if (x >= btn.x && x < btn.x + btn.width && y >= btn.y && y < btn.y + btn.height) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int Toolbar::hit_test_breadcrumb(double x, double y) const {
    if (y < 0 || y >= height_) return -1;

    for (size_t i = 0; i < breadcrumbs_.size(); i++) {
        const auto& seg = breadcrumbs_[i];
        if (x >= seg.x && x < seg.x + seg.width) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int Toolbar::hit_test_search(double x, double y) const {
    if (y < 0 || y >= height_) return -1;
    return (x >= 0) ? 0 : -1;
}

void Toolbar::update_buttons() {
    buttons_.clear();

    int x = BUTTON_PADDING;
    int y = (height_ - BUTTON_SIZE) / 2;

    buttons_.push_back({"◀", "go-previous", "Go Back", false, false, false, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING;

    buttons_.push_back({"▶", "go-next", "Go Forward", false, false, false, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING;

    buttons_.push_back({"⬆", "go-up", "Go Up", false, false, false, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING * 2;

    buttons_.push_back({"☰", "view-list", "List View", true, true, view_mode_ == ViewMode::List, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING;

    buttons_.push_back({"⊞", "view-grid", "Icon View", true, true, view_mode_ == ViewMode::Icon, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING;

    buttons_.push_back({"∥", "view-column", "Column View", true, true, view_mode_ == ViewMode::Column, x, y, BUTTON_SIZE, BUTTON_SIZE});
    x += BUTTON_SIZE + BUTTON_PADDING;

    buttons_.push_back({"🖼", "view-gallery", "Gallery View", true, true, view_mode_ == ViewMode::Gallery, x, y, BUTTON_SIZE, BUTTON_SIZE});
}

void Toolbar::update_breadcrumbs() {
    breadcrumbs_.clear();

    namespace fs = std::filesystem;
    fs::path p(current_path_);

    int x = 0;
    for (auto it = p.begin(); it != p.end(); ++it) {
        std::string segment = it->string();
        if (segment == "/") continue;

        PathSegment ps;
        ps.name = segment;
        ps.x = x;
        ps.width = segment.length() * 10 + BREADCRUMB_SEPARATORS;
        x += ps.width + BREADCRUMB_SEPARATORS;

        breadcrumbs_.push_back(std::move(ps));
    }
}

void Toolbar::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    update_buttons();

    int content_x = x + BUTTON_PADDING;
    render_buttons(renderer, content_x, y, theme);

    int breadcrumb_x = content_x + (BUTTON_SIZE + BUTTON_PADDING) * 3 + BUTTON_PADDING * 4;
    int breadcrumb_w = w - breadcrumb_x - x + x - SEARCH_WIDTH - SCOPE_WIDTH - BUTTON_PADDING * 2;
    render_breadcrumbs(renderer, breadcrumb_x, y, breadcrumb_w, theme);

    int search_x = x + w - SEARCH_WIDTH - SCOPE_WIDTH - BUTTON_PADDING * 2;
    render_search(renderer, search_x, y, SEARCH_WIDTH, theme);

    int scope_x = x + w - SCOPE_WIDTH - BUTTON_PADDING;
    render_scope_selector(renderer, scope_x, y, SCOPE_WIDTH, theme);
}

void Toolbar::render_buttons(Renderer& renderer, int x, int, const Theme& theme) {
    int bx = x;
    for (const auto& btn : buttons_) {
        Color bg = btn.enabled ? (btn.is_active ? theme.selection : theme.background_alt) : theme.background;
        renderer.rounded_rect(bx, btn.y, btn.width, btn.height, theme.corner_radius, bg);

        Color fg = btn.enabled ? theme.foreground : theme.text_muted;
        int text_x = bx + (btn.width - static_cast<int>(btn.label.length() * 10)) / 2;
        int text_y = btn.y + (btn.height - 14) / 2;
        renderer.draw_text(text_x, text_y, btn.label, theme.result_font, fg);

        bx += btn.width + BUTTON_PADDING;
    }
}

void Toolbar::render_breadcrumbs(Renderer& renderer, int x, int y, int, const Theme& theme) {
    int bx = x;
    for (size_t i = 0; i < breadcrumbs_.size(); i++) {
        const auto& seg = breadcrumbs_[i];

        std::vector<TextSegment> segments;
        segments.push_back({seg.name, theme.foreground});
        renderer.draw_text_segments(bx, y + (height_ - 14) / 2, segments, theme.result_font);

        bx += seg.width;

        if (i < breadcrumbs_.size() - 1) {
            renderer.draw_text(bx, y + (height_ - 14) / 2, "▸", theme.result_font, theme.text_muted);
            bx += BREADCRUMB_SEPARATORS;
        }
    }
}

void Toolbar::render_search(Renderer& renderer, int x, int y, int w, const Theme& theme) {
    renderer.rounded_rect(x, y + 4, w, height_ - 8, theme.corner_radius, theme.background);

    int text_x = x + 8;
    int text_y = y + (height_ - 14) / 2;

    if (search_text_.empty()) {
        renderer.draw_text(text_x, text_y, search_placeholder_, theme.result_font, theme.text_muted);
    } else {
        renderer.draw_text(text_x, text_y, search_text_, theme.result_font, theme.foreground);
    }
}

void Toolbar::render_scope_selector(Renderer& renderer, int x, int y, int w, const Theme& theme) {
    if (scopes_.empty()) return;

    renderer.rounded_rect(x, y + 4, w, height_ - 8, theme.corner_radius, theme.background);

    int text_x = x + 8;
    int text_y = y + (height_ - 14) / 2;

    std::string label = scopes_[active_scope_].name;
    renderer.draw_text(text_x, text_y, label, theme.result_font, theme.foreground);
}

void Toolbar::set_button_callback(ButtonCallback callback) {
    button_callback_ = callback;
}

void Toolbar::set_breadcrumb_callback(BreadcrumbCallback callback) {
    breadcrumb_callback_ = callback;
}

void Toolbar::set_search_callback(SearchCallback callback) {
    search_callback_ = callback;
}

void Toolbar::set_scope_callback(ScopeCallback callback) {
    scope_callback_ = callback;
}

} // namespace waylaunch
