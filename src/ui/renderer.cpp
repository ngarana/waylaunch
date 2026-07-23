#include "waylaunch/renderer.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <memory>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

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
Renderer::~Renderer() { if (backdrop_) cairo_surface_destroy(backdrop_); }

namespace {
// Separable box blur over a small premultiplied/opaque ARGB32 surface.
void box_blur_argb(cairo_surface_t* s, int radius, int passes) {
    if (radius < 1 || passes < 1) return;
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    cairo_surface_flush(s);
    unsigned char* data = cairo_image_surface_get_data(s);
    if (!data || w <= 0 || h <= 0) return;
    std::vector<unsigned char> tmp(static_cast<size_t>(stride) * h);
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y) {
            unsigned char* row = data + y * stride;
            unsigned char* out = tmp.data() + y * stride;
            for (int x = 0; x < w; ++x) {
                int b = 0, g = 0, r = 0, a = 0, cnt = 0;
                for (int dx = -radius; dx <= radius; ++dx) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= w) continue;
                    unsigned char* px = row + xx * 4;
                    b += px[0]; g += px[1]; r += px[2]; a += px[3]; ++cnt;
                }
                unsigned char* o = out + x * 4;
                o[0] = b / cnt; o[1] = g / cnt; o[2] = r / cnt; o[3] = a / cnt;
            }
        }
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                int b = 0, g = 0, r = 0, a = 0, cnt = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    unsigned char* px = tmp.data() + yy * stride + x * 4;
                    b += px[0]; g += px[1]; r += px[2]; a += px[3]; ++cnt;
                }
                unsigned char* o = data + y * stride + x * 4;
                o[0] = b / cnt; o[1] = g / cnt; o[2] = r / cnt; o[3] = a / cnt;
            }
        }
    }
    cairo_surface_mark_dirty(s);
}
} // namespace

void Renderer::set_backdrop(const uint8_t* data, int width, int height, int stride,
                            uint32_t shm_format, bool y_invert) {
    if (backdrop_) { cairo_surface_destroy(backdrop_); backdrop_ = nullptr; }
    if (!data || width <= 0 || height <= 0) return;

    // Treat the captured desktop as opaque (RGB24 ignores the 4th byte). The
    // compositor often captures opaque content with alpha=0 in ARGB8888, which as
    // premultiplied ARGB32 would render fully transparent → no visible glass.
    cairo_format_t cfmt;
    if (shm_format == 0 || shm_format == 1) cfmt = CAIRO_FORMAT_RGB24;  // A/XRGB8888
    else return;                                                        // unsupported → no glass

    cairo_surface_t* src = cairo_image_surface_create_for_data(
        const_cast<unsigned char*>(data), cfmt, width, height, stride);
    if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        if (src) cairo_surface_destroy(src);
        return;
    }

    const int factor = 8;
    int dw = std::max(1, width / factor);
    int dh = std::max(1, height / factor);
    cairo_surface_t* small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dw, dh);
    cairo_t* cr = cairo_create(small);
    cairo_scale(cr, static_cast<double>(dw) / width, static_cast<double>(dh) / height);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(src);

    box_blur_argb(small, 2, 3);

    backdrop_ = small;
    backdrop_scale_ = factor;
    backdrop_screen_w_ = width;
    backdrop_screen_h_ = height;
    backdrop_y_invert_ = y_invert;
}

bool Renderer::has_backdrop() const { return backdrop_ != nullptr; }

void Renderer::draw_backdrop(int x, int y, int w, int h, int radius) {
    if (!backdrop_ || !cairo_ || !cairo_->cr) return;
    cairo_t* cr = cairo_->cr;
    int dw = cairo_image_surface_get_width(backdrop_);
    int dh = cairo_image_surface_get_height(backdrop_);
    if (dw <= 0 || dh <= 0) return;
    double sx = static_cast<double>(backdrop_screen_w_) / dw;
    double sy = static_cast<double>(backdrop_screen_h_) / dh;

    cairo_save(cr);
    round_rect_path(cr, x, y, w, h, radius);
    cairo_clip(cr);
    if (backdrop_y_invert_) {
        cairo_translate(cr, 0, backdrop_screen_h_);
        cairo_scale(cr, sx, -sy);
    } else {
        cairo_scale(cr, sx, sy);
    }
    cairo_set_source_surface(cr, backdrop_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
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
    round_rect_path(cairo_->cr, x, y, w, h, radius);
    cairo_fill(cairo_->cr);
}

void Renderer::draw_selection_pill(int x, int y, int w, int h, int radius, const Color& accent) {
    rounded_rect(x, y, w, h, radius, Color::from_rgba(accent.r, accent.g, accent.b, 0.28));
    rounded_rect(x, y, w, h, radius, Color::from_rgba(accent.r, accent.g, accent.b, 0.6));
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

int Renderer::draw_markup(int x, int y, const std::string& markup, const RenderFontConfig& font,
                          const Color& color, int max_width, int max_lines, bool center) {
    if (!cairo_) return 0;
    PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, font.family.c_str());
    pango_font_description_set_size(desc, static_cast<int>(font.size * PANGO_SCALE));
    if (font.bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (font.italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);

    if (max_width > 0) pango_layout_set_width(layout, max_width * PANGO_SCALE);
    if (max_width > 0 && center) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    if (max_lines == 1) {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);   // single line
    } else if (max_lines > 1) {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, -max_lines);               // ≈ cap to N lines
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    } else if (max_width > 0) {
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);        // wrap, unbounded height
    }

    // Callers escape user text before adding <span> runs, so the markup is valid;
    // if Pango ever rejects it, it leaves the layout empty (blank), never crashes.
    pango_layout_set_markup(layout, markup.c_str(), -1);

    cairo_set_source_rgba(cairo_->cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cairo_->cr, x, y);
    pango_cairo_show_layout(cairo_->cr, layout);

    PangoRectangle lr;
    pango_layout_get_pixel_extents(layout, nullptr, &lr);
    int h = lr.height;
    pango_font_description_free(desc);
    g_object_unref(layout);
    return h;
}

void Renderer::draw_search_glyph(int cx, int cy, int size, const Color& color) {
    if (!cairo_) return;
    cairo_t* cr = cairo_->cr;
    double r = size / 2.0;
    cairo_set_line_width(cr, std::max(1.5, size * 0.12));
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

    // lens
    cairo_arc(cr, cx + r * 0.55, cy + r * 0.55, r * 0.6, 0, 2 * M_PI);
    cairo_stroke(cr);

    // handle
    cairo_move_to(cr, cx + r * 1.15, cy + r * 1.15);
    cairo_line_to(cr, cx + r * 1.7, cy + r * 1.7);
    cairo_stroke(cr);
}

cairo_t* Renderer::cr() const { return cairo_ ? cairo_->cr : nullptr; }

void Renderer::round_rect_path(cairo_t* cr, int x, int y, int w, int h, int radius) {
    double r = std::min({static_cast<double>(radius), w / 2.0, h / 2.0});
    if (r < 1.0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

int Renderer::text_width(const std::string& text, const RenderFontConfig& font) {
    if (!cairo_ || text.empty()) return 0;
    PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, font.family.c_str());
    pango_font_description_set_size(desc, static_cast<int>(font.size * PANGO_SCALE));
    if (font.bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text.c_str(), -1);
    // Logical (not ink) width so trailing whitespace counts — otherwise the
    // caret wouldn't advance after typing a space and the space would look lost.
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    pango_font_description_free(desc);
    g_object_unref(layout);
    return logical.width;
}

int Renderer::text_height(const RenderFontConfig& font) {
    if (!cairo_) return static_cast<int>(font.size);
    PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, font.family.c_str());
    pango_font_description_set_size(desc, static_cast<int>(font.size * PANGO_SCALE));
    if (font.bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, "Ayg", -1);   // includes ascenders + descenders
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    int h = logical.height;
    pango_font_description_free(desc);
    g_object_unref(layout);
    return h;
}

namespace {
// tiny RAII for gdk pixbuf
struct PixbufDeleter { void operator()(GdkPixbuf* p) { if (p) g_object_unref(p); } };
using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufDeleter>;

// cache of loaded cairo surfaces keyed by "name@size"
struct IconCache {
    std::unordered_map<std::string, cairo_surface_t*> map;
    ~IconCache() { for (auto& kv : map) cairo_surface_destroy(kv.second); }
};
IconCache& icon_cache() { static IconCache c; return c; }

std::string monogram_of(const std::string& label) {
    for (char c : label) {
        if (std::isalpha((unsigned char)c)) {
            char buf[2] = { static_cast<char>(std::toupper((unsigned char)c)), 0 };
            return std::string(buf);
        }
    }
    return label.empty() ? "#" : label.substr(0, 1);
}
} // namespace

cairo_surface_t* Renderer::load_icon_surface(const std::string& icon_name, int size) {
    if (icon_name.empty()) return nullptr;
    std::string key = icon_name + "@" + std::to_string(size);
    auto it = icon_cache().map.find(key);
    if (it != icon_cache().map.end()) return it->second;

    cairo_surface_t* result = nullptr;

    auto pixbuf_to_surface = [](GdkPixbuf* pb) -> cairo_surface_t* {
        int w = gdk_pixbuf_get_width(pb);
        int h = gdk_pixbuf_get_height(pb);
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return nullptr;
        unsigned char* dst = cairo_image_surface_get_data(s);
        int dstride = cairo_image_surface_get_stride(s);
        const unsigned char* src = gdk_pixbuf_get_pixels(pb);
        int sstride = gdk_pixbuf_get_rowstride(pb);
        int nch = gdk_pixbuf_get_n_channels(pb);
        bool has_alpha = gdk_pixbuf_get_has_alpha(pb);
        cairo_surface_flush(s);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const unsigned char* p = src + y * sstride + x * nch;
                unsigned char* d = dst + y * dstride + x * 4;
                unsigned char a = has_alpha ? p[3] : 255;
                d[0] = (p[2] * a) / 255;
                d[1] = (p[1] * a) / 255;
                d[2] = (p[0] * a) / 255;
                d[3] = a;
            }
        }
        cairo_surface_mark_dirty(s);
        return s;
    };

    // 1) Direct file path (png/svg)
    if (!icon_name.empty() && (icon_name[0] == '/' || icon_name.find('.') != std::string::npos)) {
        GError* err = nullptr;
        PixbufPtr pb(gdk_pixbuf_new_from_file_at_size(icon_name.c_str(), size, size, &err));
        if (err) { g_error_free(err); err = nullptr; }
        if (pb) result = pixbuf_to_surface(pb.get());
    }

    // 2) Freedesktop icon theme lookup via GTK (no screen needed)
    if (!result) {
        GtkIconTheme* theme = gtk_icon_theme_new();
        if (theme) {
            const char* name = std::getenv("GTK_ICON_THEME");
            if (name) gtk_icon_theme_set_custom_theme(theme, name);
            GdkPixbuf* raw = gtk_icon_theme_load_icon_for_scale(
                theme, icon_name.c_str(), size, 1,
                static_cast<GtkIconLookupFlags>(GTK_ICON_LOOKUP_USE_BUILTIN), nullptr);
            if (raw) {
                PixbufPtr pb(raw);
                result = pixbuf_to_surface(pb.get());
            }
            g_object_unref(theme);
        }
    }

    icon_cache().map[key] = result;
    return result;
}

void Renderer::draw_icon(int x, int y, int size, const std::string& icon_name,
                         const std::string& label, const Color& accent) {
    if (!cairo_) return;
    cairo_surface_t* surf = load_icon_surface(icon_name, size);

    int radius = std::max(4, size / 5);
    if (!surf) {
        // monogram placeholder: rounded square accent-tinted
        Color bg{accent.r, accent.g, accent.b, 0.18};
        rounded_rect(x, y, size, size, radius, bg);
        Color fg{accent.r, accent.g, accent.b, 1.0};
        std::string m = monogram_of(label.empty() ? icon_name : label);
        RenderFontConfig f{"Sans", static_cast<double>(size) * 0.5, true, false};
        PangoLayout* layout = pango_cairo_create_layout(cairo_->cr);
        PangoFontDescription* desc = pango_font_description_new();
        pango_font_description_set_family(desc, f.family.c_str());
        pango_font_description_set_size(desc, static_cast<int>(f.size * PANGO_SCALE));
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, m.c_str(), -1);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        cairo_set_source_rgba(cairo_->cr, fg.r, fg.g, fg.b, fg.a);
        cairo_move_to(cairo_->cr, x + (size - ink.width) / 2 - ink.x, y + (size - ink.height) / 2 - ink.y);
        pango_cairo_show_layout(cairo_->cr, layout);
        pango_font_description_free(desc);
        g_object_unref(layout);
        return;
    }

    // clip to rounded rect and paint the icon scaled to size
    cairo_save(cairo_->cr);
    round_rect_path(cairo_->cr, x, y, size, size, radius);
    cairo_clip(cairo_->cr);
    int sw = cairo_image_surface_get_width(surf);
    int sh = cairo_image_surface_get_height(surf);
    double scale = std::min(static_cast<double>(size) / sw, static_cast<double>(size) / sh);
    double dw = sw * scale, dh = sh * scale;
    cairo_translate(cairo_->cr, x + (size - dw) / 2, y + (size - dh) / 2);
    cairo_scale(cairo_->cr, scale, scale);
    cairo_set_source_surface(cairo_->cr, surf, 0, 0);
    cairo_paint(cairo_->cr);
    cairo_restore(cairo_->cr);
}

} // namespace waylaunch
