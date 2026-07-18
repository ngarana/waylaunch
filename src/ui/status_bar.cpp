#include "waylaunch/status_bar.h"
#include "waylaunch/toolbar.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <pango/pangocairo.h>

namespace waylaunch {

PathBar::PathBar() = default;
PathBar::~PathBar() = default;

void PathBar::set_height(int height) {
    height_ = height;
}

int PathBar::height() const {
    return height_;
}

void PathBar::set_path(const std::string& path) {
    if (path_ != path) {
        path_ = path;
        update_segments();
    }
}

const std::string& PathBar::path() const {
    return path_;
}

int PathBar::hit_test(double x, double y) const {
    if (y < 0 || y >= height_) return -1;

    for (size_t i = 0; i < segments_.size(); i++) {
        const auto& seg = segments_[i];
        if (x >= seg.x && x < seg.x + seg.width) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void PathBar::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background);

    int current_x = x + PADDING;
    int text_y = y + (h - 14) / 2;

    for (size_t i = 0; i < segments_.size(); i++) {
        const auto& seg = segments_[i];

        std::vector<TextSegment> text_segments;
        text_segments.push_back({seg.name, theme.foreground});
        renderer.draw_text_segments(current_x, text_y, text_segments, theme.result_font);

        current_x += seg.width;

        if (i < segments_.size() - 1) {
            renderer.draw_text(current_x, text_y, "/", theme.result_font, theme.text_muted);
            current_x += SEGMENT_SEPARATOR;
        }
    }
}

void PathBar::set_segment_callback(SegmentCallback callback) {
    segment_callback_ = callback;
}

void PathBar::update_segments() {
    segments_.clear();

    std::filesystem::path p(path_);

    int x = PADDING;
    for (auto it = p.begin(); it != p.end(); ++it) {
        std::string segment = it->string();
        if (segment == "/") {
            if (segments_.empty()) {
                PathSegment ps;
                ps.name = "/";
                ps.path = "/";
                ps.x = x;
                ps.width = 10;
                x += ps.width + SEGMENT_SEPARATOR;
                segments_.push_back(std::move(ps));
            }
            continue;
        }

        PathSegment ps;
        ps.name = segment;

        std::filesystem::path build_path;
        for (auto it2 = p.begin(); it2 != it; ++it2) {
            build_path /= *it2;
        }
        build_path /= segment;
        ps.path = build_path.string();

        ps.x = x;
        ps.width = static_cast<int>(segment.length() * 10);
        x += ps.width + SEGMENT_SEPARATOR;

        segments_.push_back(std::move(ps));
    }
}

StatusBar::StatusBar() = default;
StatusBar::~StatusBar() = default;

void StatusBar::set_height(int height) {
    height_ = height;
}

int StatusBar::height() const {
    return height_;
}

void StatusBar::set_item_count(int total, int selected) {
    total_items_ = total;
    selected_items_ = selected;
}

void StatusBar::set_selected_size(off_t size) {
    selected_size_ = size;
}

void StatusBar::set_available_space(off_t space) {
    available_space_ = space;
}

void StatusBar::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int text_y = y + (h - 14) / 2;

    std::ostringstream left_text;
    left_text << total_items_ << " items";
    if (available_space_ > 0) {
        left_text << ", " << format_size(available_space_) << " available";
    }
    renderer.draw_text(x + PADDING, text_y, left_text.str(), theme.result_detail_font, theme.text_muted);

    if (selected_items_ > 0) {
        std::ostringstream right_text;
        right_text << "Selected: " << selected_items_ << " items";
        if (selected_size_ > 0) {
            right_text << " (" << format_size(selected_size_) << ")";
        }

        PangoLayout* layout = pango_cairo_create_layout(nullptr);
        PangoFontDescription* desc = pango_font_description_new();
        pango_font_description_set_family(desc, theme.result_detail_font.family.c_str());
        pango_font_description_set_size(desc, static_cast<int>(theme.result_detail_font.size * PANGO_SCALE));
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, right_text.str().c_str(), -1);

        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);

        int right_x = x + w - ink.width - PADDING;
        renderer.draw_text(right_x, text_y, right_text.str(), theme.result_detail_font, theme.text_muted);

        pango_font_description_free(desc);
        g_object_unref(layout);
    }
}

std::string StatusBar::format_size(off_t size) const {
    if (size < 1024) return std::to_string(size) + " B";
    if (size < 1024 * 1024) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (size / 1024.0) << " KB";
        return oss.str();
    }
    if (size < 1024 * 1024 * 1024) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (size / (1024.0 * 1024.0)) << " MB";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (size / (1024.0 * 1024.0 * 1024.0)) << " GB";
    return oss.str();
}

} // namespace waylaunch
