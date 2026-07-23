#include "waylaunch/power/power_glyphs.h"
#include <cairo/cairo.h>
#include <cmath>

namespace waylaunch {
namespace power_glyphs {

namespace {

void begin_stroke(cairo_t* cr, const Color& c, double lw) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

// IEC power symbol: broken ring with a bar through the gap.
void draw_power(cairo_t* cr, double cx, double cy, double s) {
    double r = 0.33 * s;
    double oy = cy + 0.04 * s;
    cairo_new_path(cr);
    cairo_arc(cr, cx, oy, r, -M_PI_2 + 0.55, -M_PI_2 - 0.55 + 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, oy - r - 0.14 * s);
    cairo_line_to(cr, cx, oy - 0.04 * s);
    cairo_stroke(cr);
}

// Circular arrow: an open ring with an arrowhead at its trailing end.
void draw_restart(cairo_t* cr, double cx, double cy, double s) {
    double r = 0.33 * s;
    double start = -M_PI_2 + 0.95;
    double end = -M_PI_2 - 0.30 + 2 * M_PI;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, start, end);
    cairo_stroke(cr);

    // Arrowhead: a filled triangle at the arc end, pointing along the tangent.
    double te = end;
    double px = cx + r * std::cos(te), py = cy + r * std::sin(te);
    double dx = -std::sin(te), dy = std::cos(te);      // direction of travel
    double nx = std::cos(te), ny = std::sin(te);       // radial normal
    double a = 0.17 * s;
    cairo_new_path(cr);
    cairo_move_to(cr, px + dx * a, py + dy * a);
    cairo_line_to(cr, px + nx * a * 0.62, py + ny * a * 0.62);
    cairo_line_to(cr, px - nx * a * 0.62, py - ny * a * 0.62);
    cairo_close_path(cr);
    cairo_fill(cr);
}

// Padlock: stroked shackle over a filled, rounded body.
void draw_lock(Renderer& renderer, cairo_t* cr, double cx, double cy, double s) {
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - 0.08 * s, 0.17 * s, M_PI, 2 * M_PI);
    cairo_stroke(cr);
    renderer.round_rect_path(cr, static_cast<int>(cx - 0.26 * s), static_cast<int>(cy - 0.08 * s),
                             static_cast<int>(0.52 * s), static_cast<int>(0.44 * s),
                             static_cast<int>(0.09 * s));
    cairo_fill(cr);
}

// Logout: an open door frame with an arrow leaving through it.
void draw_exit(cairo_t* cr, double cx, double cy, double s) {
    cairo_new_path(cr);
    cairo_move_to(cr, cx + 0.02 * s, cy - 0.34 * s);
    cairo_line_to(cr, cx - 0.30 * s, cy - 0.34 * s);
    cairo_line_to(cr, cx - 0.30 * s, cy + 0.34 * s);
    cairo_line_to(cr, cx + 0.02 * s, cy + 0.34 * s);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - 0.06 * s, cy);
    cairo_line_to(cr, cx + 0.40 * s, cy);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + 0.24 * s, cy - 0.15 * s);
    cairo_line_to(cr, cx + 0.40 * s, cy);
    cairo_line_to(cr, cx + 0.24 * s, cy + 0.15 * s);
    cairo_stroke(cr);
}

// Crescent moon (filled), tilted like the usual sleep glyph.
void draw_suspend(cairo_t* cr, double cx, double cy, double s) {
    double R = 0.36 * s;
    double d = 0.55 * R;
    double Ri = std::sqrt(d * d + R * R);
    double phi = std::atan2(R, d);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, -0.5);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, R, -M_PI_2, M_PI_2);            // outer bulge (right half)
    cairo_arc_negative(cr, -d, 0, Ri, phi, -phi);       // inner carve through both tips
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

// Suspend-to-disk: an arrow descending onto a baseline.
void draw_hibernate(cairo_t* cr, double cx, double cy, double s) {
    cairo_new_path(cr);
    cairo_move_to(cr, cx - 0.30 * s, cy + 0.34 * s);
    cairo_line_to(cr, cx + 0.30 * s, cy + 0.34 * s);
    cairo_stroke(cr);

    cairo_move_to(cr, cx, cy - 0.36 * s);
    cairo_line_to(cr, cx, cy + 0.10 * s);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 0.16 * s, cy - 0.06 * s);
    cairo_line_to(cr, cx, cy + 0.10 * s);
    cairo_line_to(cr, cx + 0.16 * s, cy - 0.06 * s);
    cairo_stroke(cr);
}

} // namespace

void draw(Renderer& renderer, const std::string& action_id,
          double cx, double cy, double size, const Color& color) {
    cairo_t* cr = renderer.cr();
    if (!cr) return;
    cairo_save(cr);
    begin_stroke(cr, color, std::max(1.6, size * 0.085));

    if (action_id == "lock")            draw_lock(renderer, cr, cx, cy, size);
    else if (action_id == "restart")    draw_restart(cr, cx, cy, size);
    else if (action_id == "exit")       draw_exit(cr, cx, cy, size);
    else if (action_id == "hibernate")  draw_hibernate(cr, cx, cy, size);
    else if (action_id == "suspend")    draw_suspend(cr, cx, cy, size);
    else                                draw_power(cr, cx, cy, size);   // shutdown + fallback

    cairo_restore(cr);
}

} // namespace power_glyphs
} // namespace waylaunch
