#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#include <cairo/cairo.h>

namespace waylaunch {

struct Color {
    double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
    static Color from_hex(const std::string& hex);
    static Color from_rgba(double r, double g, double b, double a);
};

struct RenderFontConfig {
    std::string family = "Sans";
    double size = 14.0;
    bool bold = false;
    bool italic = false;
};

struct Theme {
    Color background;
    Color background_alt;
    Color foreground;
    Color text_muted;
    Color accent;
    Color accent_hover;
    Color error;
    Color warning;
    Color success;
    Color border;
    Color selection;
    RenderFontConfig input_font;
    RenderFontConfig result_font;
    RenderFontConfig result_detail_font;
    int corner_radius = 0;
    double opacity = 1.0;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    void begin(uint8_t* data, int stride, int width, int height);
    void end();
    void clear(const Color& color);
    void fill_rect(int x, int y, int w, int h, const Color& color);
    void rounded_rect(int x, int y, int w, int h, int radius, const Color& color);
    // Accent selection pill (fill + brighter overlay) — the one treatment shared
    // by the switcher and power HUD cards, factored here so it can't diverge.
    void draw_selection_pill(int x, int y, int w, int h, int radius, const Color& accent);
    void draw_text(int x, int y, const std::string& text, const RenderFontConfig& font, const Color& color);
    // Draw Pango-markup text (supports <span>, <b>, …) with `color` as the base.
    // max_width>0 constrains width (in px); max_lines: 0 = unlimited (wrap if
    // width set), 1 = single line ellipsized, >1 = wrap capped to N lines then
    // ellipsized. center=true centers lines within max_width (dialog layouts).
    // Returns the pixel height drawn (for stacking).
    int draw_markup(int x, int y, const std::string& markup, const RenderFontConfig& font,
                    const Color& color, int max_width = 0, int max_lines = 0,
                    bool center = false);
    // Draw a macOS-style search (magnifier) glyph at (cx, cy).
    void draw_search_glyph(int cx, int cy, int size, const Color& color);

    // Draw a rounded icon: tries to load the freedesktop icon by name (or an
    // absolute/relative image path) via gdk-pixbuf, falls back to a monogram
    // drawn from `label` on an accent-tinted rounded square.
    void draw_icon(int x, int y, int size, const std::string& icon_name,
                   const std::string& label, const Color& accent);

    // Load an icon into a cairo surface (cached). Returns nullptr if unavailable.
    cairo_surface_t* load_icon_surface(const std::string& icon_name, int size);

    // Path-only rounded rectangle (for clipping).
    void round_rect_path(cairo_t* cr, int x, int y, int w, int h, int radius);

    // The active cairo context between begin()/end() (nullptr otherwise) — for
    // feature renderers drawing custom vector paths (glyphs, arcs) with the
    // toolkit primitives above as a base.
    cairo_t* cr() const;

    // Measure pixel width of text in the given font (for caret placement).
    int text_width(const std::string& text, const RenderFontConfig& font);
    // Logical line height of the font (for vertical centering).
    int text_height(const RenderFontConfig& font);

    // --- Frosted-glass backdrop (client-side blur) ---
    // Supply the captured screen (full output). Builds a downsampled+blurred
    // cache. `shm_format` is a wl_shm format (ARGB8888=0, XRGB8888=1).
    void set_backdrop(const uint8_t* data, int width, int height, int stride,
                      uint32_t shm_format, bool y_invert);
    bool has_backdrop() const;
    // Paint the blurred backdrop clipped to a rounded rect at screen (x,y,w,h),
    // aligned so it samples the backdrop at those screen coordinates.
    void draw_backdrop(int x, int y, int w, int h, int radius);

private:
    struct CairoState;
    std::unique_ptr<CairoState> cairo_;

    cairo_surface_t* backdrop_ = nullptr;   // downsampled + blurred
    int backdrop_scale_ = 1;                 // downsample factor
    int backdrop_screen_w_ = 0;
    int backdrop_screen_h_ = 0;
    bool backdrop_y_invert_ = false;
};

} // namespace waylaunch
