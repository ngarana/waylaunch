#include "waylaunch/tabs.h"
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <ctime>

namespace waylaunch {

Tabs::Tabs() = default;
Tabs::~Tabs() = default;

void Tabs::set_height(int height) {
    height_ = height;
}

int Tabs::height() const {
    return height_;
}

std::string Tabs::add_tab(const std::string& path, bool activate) {
    Tab tab;
    tab.id = generate_id();
    tab.path = path;
    tab.title = std::filesystem::path(path).filename().string();
    if (tab.title.empty()) tab.title = path;
    tab.is_active = false;

    tabs_.push_back(std::move(tab));

    if (activate && tabs_.size() == 1) {
        activate_tab(tabs_.back().id);
    }

    // Keep positions up-to-date so hit_test works without a prior render call.
    update_tab_positions(static_cast<int>(tabs_.size()) * TAB_MAX_WIDTH + NEW_TAB_BUTTON_WIDTH);

    return tabs_.back().id;
}

void Tabs::close_tab(const std::string& tab_id) {
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
        [&](const Tab& t) { return t.id == tab_id; });

    if (it == tabs_.end()) return;

    bool was_active = it->is_active;
    int index = static_cast<int>(std::distance(tabs_.begin(), it));

    tabs_.erase(it);

    if (was_active && !tabs_.empty()) {
        int new_index = std::min(index, static_cast<int>(tabs_.size()) - 1);
        activate_tab(tabs_[new_index].id);
    } else if (!was_active && index < active_index_) {
        // A tab before the active one was removed; keep the same tab active.
        active_index_--;
    }

    if (tab_closed_callback_) {
        tab_closed_callback_(tab_id);
    }
}

void Tabs::close_tab(int index) {
    if (index >= 0 && index < static_cast<int>(tabs_.size())) {
        close_tab(tabs_[index].id);
    }
}

void Tabs::close_other_tabs(const std::string& tab_id) {
    std::string active_id = tab_id;
    tabs_.erase(
        std::remove_if(tabs_.begin(), tabs_.end(),
            [&](const Tab& t) { return t.id != tab_id; }),
        tabs_.end());

    if (!tabs_.empty()) {
        activate_tab(tab_id);
    }
}

void Tabs::activate_tab(const std::string& tab_id) {
    for (auto& tab : tabs_) {
        tab.is_active = (tab.id == tab_id);
    }

    active_index_ = -1;
    for (size_t i = 0; i < tabs_.size(); i++) {
        if (tabs_[i].id == tab_id) {
            active_index_ = static_cast<int>(i);
            break;
        }
    }

    if (tab_changed_callback_) {
        tab_changed_callback_(tab_id);
    }
}

void Tabs::activate_tab(int index) {
    if (index >= 0 && index < static_cast<int>(tabs_.size())) {
        activate_tab(tabs_[index].id);
    }
}

void Tabs::activate_next_tab() {
    if (tabs_.empty()) return;
    int next = (active_index_ + 1) % tabs_.size();
    activate_tab(next);
}

void Tabs::activate_previous_tab() {
    if (tabs_.empty()) return;
    int prev = (active_index_ - 1 + tabs_.size()) % tabs_.size();
    activate_tab(prev);
}

int Tabs::tab_count() const {
    return static_cast<int>(tabs_.size());
}

int Tabs::active_tab_index() const {
    return active_index_;
}

const Tab* Tabs::get_tab(const std::string& tab_id) const {
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
        [&](const Tab& t) { return t.id == tab_id; });
    return (it != tabs_.end()) ? &(*it) : nullptr;
}

const Tab* Tabs::get_tab(int index) const {
    if (index >= 0 && index < static_cast<int>(tabs_.size())) {
        return &tabs_[index];
    }
    return nullptr;
}

const Tab* Tabs::active_tab() const {
    if (active_index_ >= 0 && active_index_ < static_cast<int>(tabs_.size())) {
        return &tabs_[active_index_];
    }
    return nullptr;
}

std::string Tabs::active_tab_id() const {
    const Tab* tab = active_tab();
    return tab ? tab->id : "";
}

std::string Tabs::active_tab_path() const {
    const Tab* tab = active_tab();
    return tab ? tab->path : "";
}

void Tabs::set_tab_path(const std::string& tab_id, const std::string& path) {
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
        [&](const Tab& t) { return t.id == tab_id; });

    if (it != tabs_.end()) {
        it->path = path;
        it->title = std::filesystem::path(path).filename().string();
        if (it->title.empty()) it->title = path;
    }
}

int Tabs::hit_test(double x, double y) const {
    if (y < 0 || y >= height_) return -1;

    for (size_t i = 0; i < tabs_.size(); i++) {
        const auto& tab = tabs_[i];
        if (x >= tab.x && x < tab.x + tab.width) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int Tabs::hit_test_close_button(double x, double y) const {
    if (y < 0 || y >= height_) return -1;

    for (size_t i = 0; i < tabs_.size(); i++) {
        const auto& tab = tabs_[i];
        int close_x = tab.x + tab.width - CLOSE_BUTTON_SIZE - CLOSE_BUTTON_PADDING;
        int close_y = (height_ - CLOSE_BUTTON_SIZE) / 2;

        if (x >= close_x && x < close_x + CLOSE_BUTTON_SIZE &&
            y >= close_y && y < close_y + CLOSE_BUTTON_SIZE) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void Tabs::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    update_tab_positions(w - NEW_TAB_BUTTON_WIDTH);

    for (size_t i = 0; i < tabs_.size(); i++) {
        const auto& tab = tabs_[i];

        Color bg = tab.is_active ? theme.background : theme.background_alt;
        renderer.fill_rect(x + tab.x, y, tab.width, h - 1, bg);

        if (tab.is_active) {
            renderer.fill_rect(x + tab.x, y + h - 2, tab.width, 2, theme.accent);
        }

        int text_x = x + tab.x + TAB_PADDING;

        std::vector<TextSegment> segments;
        segments.push_back({tab.title, tab.is_active ? theme.foreground : theme.text_muted});
        renderer.draw_text_segments(text_x, y + (h - 14) / 2, segments, theme.result_font);

        int close_x = x + tab.x + tab.width - CLOSE_BUTTON_SIZE - CLOSE_BUTTON_PADDING;
        int close_y = y + (h - CLOSE_BUTTON_SIZE) / 2;
        renderer.draw_text(close_x, close_y, "×", theme.result_font, theme.text_muted);
    }

    int new_tab_x = x + w - NEW_TAB_BUTTON_WIDTH;
    renderer.draw_text(new_tab_x + 8, y + (h - 14) / 2, "+", theme.result_font, theme.accent);
}

std::string Tabs::generate_id() const {
    static int counter = 0;
    std::ostringstream oss;
    oss << "tab_" << std::time(nullptr) << "_" << counter++;
    return oss.str();
}

void Tabs::update_tab_positions(int available_width) {
    int total_width = 0;
    for (const auto& tab : tabs_) {
        int title_width = static_cast<int>(tab.title.length() * 8 + 2 * TAB_PADDING + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_PADDING);
        total_width += std::max(TAB_MIN_WIDTH, std::min(TAB_MAX_WIDTH, title_width));
    }

    int x = 0;
    for (auto& tab : tabs_) {
        int title_width = static_cast<int>(tab.title.length() * 8 + 2 * TAB_PADDING + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_PADDING);
        tab.width = std::max(TAB_MIN_WIDTH, std::min(TAB_MAX_WIDTH, title_width));

        if (total_width > available_width) {
            tab.width = available_width / tabs_.size();
        }

        tab.x = x;
        x += tab.width;
    }
}

void Tabs::set_tab_changed_callback(TabChangedCallback callback) {
    tab_changed_callback_ = callback;
}

void Tabs::set_tab_closed_callback(TabClosedCallback callback) {
    tab_closed_callback_ = callback;
}

} // namespace waylaunch
