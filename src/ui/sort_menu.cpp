#include "waylaunch/sort_menu.h"
#include <algorithm>

namespace waylaunch {

SortMenu::SortMenu() {
    build_menu_items();
}

SortMenu::~SortMenu() = default;

void SortMenu::set_visible(bool visible) {
    visible_ = visible;
}

bool SortMenu::is_visible() const {
    return visible_;
}

void SortMenu::set_position(int x, int y) {
    x_ = x;
    y_ = y;
}

void SortMenu::set_sort_field(SortField field) {
    current_field_ = field;
    for (auto& item : sort_items_) {
        item.selected = (item.field == field);
    }
}

void SortMenu::set_sort_order(SortOrder order) {
    current_order_ = order;
}

void SortMenu::set_group_mode(GroupMode mode) {
    current_group_ = mode;
    for (auto& item : group_items_) {
        item.selected = (item.mode == mode);
    }
}

int SortMenu::hit_test(double x, double y) const {
    if (!visible_) return -1;

    if (x < x_ || x >= x_ + MENU_WIDTH) return -1;
    if (y < y_ || y >= y_ + get_height()) return -1;

    int current_y = y_ + PADDING;

    for (size_t i = 0; i < sort_items_.size(); i++) {
        if (y >= current_y && y < current_y + ITEM_HEIGHT) {
            return static_cast<int>(i);
        }
        current_y += ITEM_HEIGHT;
    }

    current_y += 8;

    for (size_t i = 0; i < group_items_.size(); i++) {
        if (y >= current_y && y < current_y + ITEM_HEIGHT) {
            return static_cast<int>(sort_items_.size() + i);
        }
        current_y += ITEM_HEIGHT;
    }

    return -1;
}

void SortMenu::render(Renderer& renderer, int, int, int, int, const Theme& theme) {
    if (!visible_) return;

    int height = get_height();
    renderer.rounded_rect(x_, y_, MENU_WIDTH, height, theme.corner_radius, theme.background_alt);
    renderer.fill_rect(x_, y_, MENU_WIDTH, 1, theme.border);

    int current_y = y_ + PADDING;

    renderer.draw_text(x_ + PADDING, current_y, "Sort By", theme.result_detail_font, theme.text_muted);
    current_y += ITEM_HEIGHT;

    for (size_t i = 0; i < sort_items_.size(); i++) {
        const auto& item = sort_items_[i];

        if (item.selected) {
            renderer.fill_rect(x_ + 2, current_y, MENU_WIDTH - 4, ITEM_HEIGHT, theme.selection);
        }

        std::vector<TextSegment> segments;
        if (item.selected) {
            segments.push_back({"✓ ", theme.accent});
        }
        segments.push_back({item.label, theme.foreground});
        renderer.draw_text_segments(x_ + PADDING, current_y + 6, segments, theme.result_font);

        current_y += ITEM_HEIGHT;
    }

    current_y += 8;

    renderer.draw_text(x_ + PADDING, current_y, "Group By", theme.result_detail_font, theme.text_muted);
    current_y += ITEM_HEIGHT;

    for (size_t i = 0; i < group_items_.size(); i++) {
        const auto& item = group_items_[i];

        if (item.selected) {
            renderer.fill_rect(x_ + 2, current_y, MENU_WIDTH - 4, ITEM_HEIGHT, theme.selection);
        }

        std::vector<TextSegment> segments;
        if (item.selected) {
            segments.push_back({"✓ ", theme.accent});
        }
        segments.push_back({item.label, theme.foreground});
        renderer.draw_text_segments(x_ + PADDING, current_y + 6, segments, theme.result_font);

        current_y += ITEM_HEIGHT;
    }
}

void SortMenu::set_sort_callback(SortCallback callback) {
    sort_callback_ = callback;
}

void SortMenu::set_group_callback(GroupCallback callback) {
    group_callback_ = callback;
}

void SortMenu::build_menu_items() {
    sort_items_.clear();
    sort_items_.push_back(SortMenuItem{"Name", SortField::Name, true});
    sort_items_.push_back(SortMenuItem{"Date Modified", SortField::DateModified, false});
    sort_items_.push_back(SortMenuItem{"Date Created", SortField::DateCreated, false});
    sort_items_.push_back(SortMenuItem{"Size", SortField::Size, false});
    sort_items_.push_back(SortMenuItem{"Kind", SortField::Kind, false});

    group_items_.clear();
    group_items_.push_back(GroupMenuItem{"None", GroupMode::None, true});
    group_items_.push_back(GroupMenuItem{"Kind", GroupMode::Kind, false});
    group_items_.push_back(GroupMenuItem{"Date Modified", GroupMode::DateModified, false});
    group_items_.push_back(GroupMenuItem{"Size", GroupMode::Size, false});
}

int SortMenu::get_height() const {
    return PADDING + ITEM_HEIGHT + sort_items_.size() * ITEM_HEIGHT + 8 + ITEM_HEIGHT + group_items_.size() * ITEM_HEIGHT + PADDING;
}

} // namespace waylaunch
