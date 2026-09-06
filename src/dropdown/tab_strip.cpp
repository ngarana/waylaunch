#include "waylaunch/dropdown/tab_strip.h"

#include <algorithm>

namespace waylaunch {
namespace {

constexpr int kTabPadding = 12;
constexpr int kMinTabWidth = 48;

// Titles are arbitrary client strings; escape the Pango markup metacharacters
// before single-line ellipsized drawing.
std::string escape_markup(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

void TabStrip::update(std::vector<Tab> tabs) { tabs_ = std::move(tabs); }

std::vector<TabStrip::Rect> TabStrip::layout(int total_width) const {
    std::vector<Rect> rects;
    if (tabs_.empty() || total_width <= 0) return rects;
    int width = std::max(kMinTabWidth, total_width / static_cast<int>(tabs_.size()));
    for (size_t i = 0; i < tabs_.size(); ++i) {
        rects.push_back({.x = static_cast<int>(i) * width, .y = 0, .w = width, .h = kHeight});
    }
    return rects;
}

uintptr_t TabStrip::hit_test(int x, int y, int total_width) const {
    if (y < 0 || y >= kHeight) return 0;
    std::vector<Rect> rects = layout(total_width);
    for (size_t i = 0; i < tabs_.size() && i < rects.size(); ++i) {
        if (rects[i].contains(x, y)) return tabs_[i].handle_id;
    }
    return 0;
}

void TabStrip::render(Renderer& renderer, int total_width, const Colors& colors,
                      const RenderFontConfig& font) const {
    renderer.clear(colors.background);
    if (tabs_.empty()) return;
    std::vector<Rect> rects = layout(total_width);
    int text_y = (kHeight - renderer.text_height(font)) / 2;
    for (size_t i = 0; i < tabs_.size() && i < rects.size(); ++i) {
        const Rect& rect = rects[i];
        const Color& text = tabs_[i].is_active ? colors.background : colors.foreground;
        if (tabs_[i].is_active) {
            renderer.rounded_rect(rect.x + 2, 3, rect.w - 4, kHeight - 6, 8, colors.accent);
        }
        renderer.draw_markup(rect.x + kTabPadding, text_y, escape_markup(tabs_[i].title), font,
                             text, rect.w - (2 * kTabPadding), 1);
    }
}

} // namespace waylaunch
