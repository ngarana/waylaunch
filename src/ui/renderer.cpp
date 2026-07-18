#include "waylaunch/renderer.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace waylaunch {

Color Color::from_hex(const std::string& hex) {
    Color c;
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() >= 6) {
        c.r = std::stoi(h.substr(0, 2), nullptr, 16) / 255.0;
        c.g = std::stoi(h.substr(2, 2), nullptr, 16) / 255.0;
        c.b = std::stoi(h.substr(4, 2), nullptr, 16) / 255.0;
        c.a = (h.size() >= 8) ? std::stoi(h.substr(6, 2), nullptr, 16) / 255.0 : 1.0;
    }
    return c;
}

Color Color::from_rgba(double r, double g, double b, double a) { return {r, g, b, a}; }

struct Renderer::CairoState {
    cairo_surface_t* surface = nullptr;
    cairo_t* cr = nullptr;

    ~CairoState() {
        if (cr) { cairo_destroy(cr); cr = nullptr; }
        if (surface) { cairo_surface_destroy(surface); surface = nullptr; }
    }
};

Renderer::Renderer() : cairo_(std::make_unique<CairoState>()) {}
Renderer::~Renderer() = default;

int Renderer::stride_for_width(int width) const {
    return cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
}

void Renderer::begin(uint8_t* data, int stride, int width, int height) {
    // Destroy previous state
    cairo_.reset();
    cairo_ = std::make_unique<CairoState>();

    cairo_->surface = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_->cr = cairo_create(cairo_->surface);
}

void Renderer::end() {
    if (cairo_ && cairo_->surface) {
        cairo_surface_flush(cairo_->surface);
    }
    cairo_.reset();
}

void Renderer::clear(const Color& color) {
    cairo_set_operator(cairo_->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo_->cr, color.r, color.g, color.b, color.a);
    cairo_paint(cairo_->cr);
    cairo_set_operator(cairo_->cr, CAIRO_OPERATOR_OVER);
}

void Renderer::fill_rect(int x, int y, int w, int h, const Color& color) {
    cairo_set_source_rgba(cairo_->cr, color.r, color.g, color.b, color.a);
    cairo_rectangle(cairo_->cr, x, y, w, h);
    cairo_fill(cairo_->cr);
}

void Renderer::rounded_rect(int x, int y, int w, int h, int radius, const Color& color) {
    cairo_set_source_rgba(cairo_->cr, color.r, color.g, color.b, color.a);
    double r = std::min({static_cast<double>(radius), w / 2.0, h / 2.0});
    if (r < 1.0) {
        cairo_rectangle(cairo_->cr, x, y, w, h);
        cairo_fill(cairo_->cr);
        return;
    }
    cairo_new_sub_path(cairo_->cr);
    cairo_arc(cairo_->cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cairo_->cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cairo_->cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cairo_->cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cairo_->cr);
    cairo_fill(cairo_->cr);
}

void Renderer::draw_text(int x, int y, const std::string& text, const RenderFontConfig& font, const Color& color) {
    PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, font.family.c_str());
    pango_font_description_set_size(desc, static_cast<int>(font.size * PANGO_SCALE));
    if (font.bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (font.italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text.c_str(), -1);
    cairo_set_source_rgba(cairo_->cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cairo_->cr, x, y);
    pango_cairo_show_layout(cairo_->cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

void Renderer::draw_text_segments(int x, int y, const std::vector<TextSegment>& segments, const RenderFontConfig& font) {
    PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, font.family.c_str());
    pango_font_description_set_size(desc, static_cast<int>(font.size * PANGO_SCALE));
    pango_layout_set_font_description(layout, desc);

    int cx = x;
    for (const auto& seg : segments) {
        pango_layout_set_text(layout, seg.text.c_str(), -1);
        cairo_set_source_rgba(cairo_->cr, seg.color.r, seg.color.g, seg.color.b, seg.color.a);
        cairo_move_to(cairo_->cr, cx, y);
        pango_cairo_show_layout(cairo_->cr, layout);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        cx += ink.width;
    }
    pango_font_description_free(desc);
    g_object_unref(layout);
}

void Renderer::draw_input_box(int x, int y, int w, int h, const std::string& text,
                               size_t cursor_pos, const Theme& theme) {
    rounded_rect(x, y, w, h, theme.corner_radius, theme.background_alt);
    cairo_set_source_rgba(cairo_->cr, theme.border.r, theme.border.g, theme.border.b, theme.border.a);
    cairo_set_line_width(cairo_->cr, 1.0);
    cairo_rectangle(cairo_->cr, x + 0.5, y + 0.5, w - 1, h - 1);
    cairo_stroke(cairo_->cr);

    int text_x = x + theme.corner_radius + 8;
    int text_y = y + (h - static_cast<int>(theme.input_font.size)) / 2;

    if (text.empty()) {
        draw_text(text_x, text_y, "Type to search...", theme.input_font, theme.text_muted);
    } else {
        draw_text(text_x, text_y, text, theme.input_font, theme.foreground);
    }

    // Cursor
    if (!text.empty() && cursor_pos <= text.size()) {
        PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
        PangoFontDescription* desc = pango_font_description_new();
        pango_font_description_set_family(desc, theme.input_font.family.c_str());
        pango_font_description_set_size(desc, static_cast<int>(theme.input_font.size * PANGO_SCALE));
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, text.substr(0, cursor_pos).c_str(), -1);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        int cursor_x = text_x + ink.width;
        cairo_set_source_rgba(cairo_->cr, theme.accent.r, theme.accent.g, theme.accent.b, theme.accent.a);
        cairo_set_line_width(cairo_->cr, 2.0);
        cairo_move_to(cairo_->cr, cursor_x, y + 6);
        cairo_line_to(cairo_->cr, cursor_x, y + h - 6);
        cairo_stroke(cairo_->cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
    }
}

void Renderer::draw_scrollbar(int x, int y, int h, int total_items, int visible_items,
                               int scroll_offset, const Color& color) {
    if (total_items <= visible_items) return;
    int thumb_height = std::max(20, (h * visible_items) / total_items);
    int thumb_y = y + (scroll_offset * (h - thumb_height)) / (total_items - visible_items);
    fill_rect(x, y, 4, h, Color{color.r, color.g, color.b, color.a * 0.2});
    rounded_rect(x, thumb_y, 4, thumb_height, 2, color);
}

void Renderer::render_into(uint8_t* buffer_data, int buffer_stride,
                            int width, int height, double /*scale*/,
                            const Theme& theme, const LayoutMetrics& layout,
                            const std::string& input_text, size_t cursor_pos,
                            const std::vector<RenderItem>& items,
                            int scroll_offset, int selected_index) {
    begin(buffer_data, buffer_stride, width, height);

    clear(theme.background);

    int input_x = layout.padding;
    int input_y = layout.padding;
    int input_w = width - 2 * layout.padding;
    int input_h = layout.input_height;
    draw_input_box(input_x, input_y, input_w, input_h, input_text, cursor_pos, theme);

    int items_y = input_y + input_h + layout.padding;
    int content_h = height - items_y - layout.padding;
    int visible_items = std::min(layout.max_visible_items, static_cast<int>(items.size()));
    if (visible_items < 1 && !items.empty()) visible_items = 1;
    int list_h = visible_items * layout.item_height;

    // Items background
    if (visible_items > 0) {
        rounded_rect(layout.padding, items_y, width - 2 * layout.padding,
                     list_h, theme.corner_radius, theme.background_alt);
    }

    for (int i = 0; i < visible_items; i++) {
        int item_idx = scroll_offset + i;
        if (item_idx >= static_cast<int>(items.size())) break;
        const auto& item = items[item_idx];
        int item_y = items_y + i * layout.item_height;

        if (item_idx == selected_index) {
            rounded_rect(layout.padding + 2, item_y + 2,
                         width - 2 * layout.padding - 4, layout.item_height - 4,
                         std::max(1, theme.corner_radius / 2), theme.selection);
        }

        int text_x = layout.padding + layout.icon_size + layout.padding * 2;
        int text_y = item_y + (layout.item_height - static_cast<int>(theme.result_font.size)) / 2;
        draw_text_segments(text_x, text_y, item.segments, theme.result_font);
    }

    if (static_cast<int>(items.size()) > visible_items) {
        draw_scrollbar(width - layout.padding - 8, items_y, list_h,
                       static_cast<int>(items.size()), visible_items, scroll_offset, theme.accent);
    }

    end();
}

} // namespace waylaunch
