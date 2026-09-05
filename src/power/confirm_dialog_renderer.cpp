#include "waylaunch/power/confirm_dialog_renderer.h"
#include "waylaunch/power/power_glyphs.h"
#include "waylaunch/power/power_layout.h"
#include <cairo/cairo.h>
#include <cmath>

namespace waylaunch {

namespace {

// Escape dialog strings for draw_markup (they come from config and may hold
// '&'/'<'), so user text can never corrupt the markup.
std::string escape_markup(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            default: o += c;
        }
    }
    return o;
}

} // namespace

void ConfirmDialogRenderer::render(Renderer& renderer, const ConfirmDialog& dialog,
                                   const Theme& theme, int screen_w, int screen_h,
                                   double font_scale) {
    if (!dialog.is_open()) return;
    const PowerAction& action = dialog.action();

    // Geometry from the shared layout (single source of truth with hit-testing).
    auto lay = power_layout::dialog(screen_w, screen_h);
    const int x = lay.card.x;
    const int y = lay.card.y;
    const int card_w = lay.card.w;
    const int card_h = lay.card.h;
    const int corner_radius = lay.corner_radius; // "mostly round" — softer than the HUD
    const int pad = lay.pad;

    // Glassmorphic card: the blurred desktop clipped to the card, a light tint
    // for contrast, a hairline border, and a top rim highlight — the same glass
    // recipe as the launcher panel, not an opaque slab.
    if (renderer.has_backdrop()) {
        renderer.draw_backdrop(x, y, card_w, card_h, corner_radius);
        renderer.rounded_rect(x, y, card_w, card_h, corner_radius,
                              Color::from_rgba(0.08, 0.08, 0.12, 0.60));
    } else {
        renderer.rounded_rect(x, y, card_w, card_h, corner_radius,
                              Color::from_rgba(0.1, 0.1, 0.14, 0.92));
    }
    renderer.rounded_rect(x, y, card_w, card_h, corner_radius,
                          Color::from_rgba(1.0, 1.0, 1.0, 0.10));
    renderer.fill_rect(x + corner_radius, y, card_w - (2 * corner_radius), 1,
                       Color::from_rgba(1.0, 1.0, 1.0, 0.22));

    int inner_x = x + pad;
    int inner_w = card_w - (2 * pad);

    // Session-ending actions are red; hibernate/suspend use the accent.
    const Color& tone = action.subtext.empty() ? theme.accent : theme.error;

    // --- Round action badge with the glyph, ringed by the countdown ---
    double icx = x + (card_w / 2.0);
    double icy = y + 64.0;
    constexpr int badge_r = 28;
    renderer.rounded_rect(static_cast<int>(icx) - badge_r, static_cast<int>(icy) - badge_r,
                          badge_r * 2, badge_r * 2, badge_r,
                          Color::from_rgba(tone.r, tone.g, tone.b, 0.20));
    power_glyphs::draw(renderer, action.id, icx, icy, badge_r * 1.35,
                       Color::from_rgba(tone.r, tone.g, tone.b, 0.98));

    if (dialog.has_countdown()) {
        // Depleting ring, from 12 o'clock clockwise, over a faint track.
        cairo_t* cr = renderer.cr();
        double ring_r = badge_r + 7.0;
        cairo_save(cr);
        cairo_set_line_width(cr, 3.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_arc(cr, icx, icy, ring_r, 0, 2 * M_PI);
        cairo_stroke(cr);
        double frac = dialog.remaining_fraction();
        if (frac > 0.0) {
            cairo_set_source_rgba(cr, tone.r, tone.g, tone.b, 0.95);
            cairo_arc(cr, icx, icy, ring_r, -M_PI_2, -M_PI_2 + (2 * M_PI * frac));
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    // --- Centered headline + optional subtext ---
    RenderFontConfig headline = theme.result_font;
    headline.size = 17.0 * font_scale;
    headline.bold = true;
    int cy = y + 64 + badge_r + 22;
    cy += renderer.draw_markup(inner_x, cy, escape_markup(action.confirm_text), headline,
                               theme.foreground, inner_w, 2, true) +
          8;

    if (!action.subtext.empty()) {
        RenderFontConfig body = theme.result_detail_font;
        body.size *= font_scale;
        renderer.draw_markup(inner_x, cy, escape_markup(action.subtext), body, theme.text_muted,
                             inner_w, 2, true);
    }

    // --- Fully-round pill buttons; the focused one is filled, the other quiet.
    //     Rects come from the shared layout so a click lands exactly where the
    //     pill is drawn. ←/→/Tab (or hover) move focus, Return/Space (or click)
    //     press it. The counter lives in the confirm label ("Shut Down · 42"),
    //     language-neutral. ---
    RenderFontConfig btn_font = theme.result_font;
    btn_font.size = 14.0 * font_scale;
    btn_font.bold = true;
    int btn_text_h = renderer.text_height(btn_font);

    auto draw_button = [&](const power_layout::Rect& b, const std::string& label, const Color& fill,
                           const Color& text, bool focused) {
        if (focused) { // 2px halo ring behind the pill
            renderer.rounded_rect(b.x - 3, b.y - 3, b.w + 6, b.h + 6, (b.h + 6) / 2,
                                  Color::from_rgba(1.0, 1.0, 1.0, 0.30));
        }
        renderer.rounded_rect(b.x, b.y, b.w, b.h, b.h / 2, fill);
        int tw = renderer.text_width(label, btn_font);
        renderer.draw_text(b.x + ((b.w - tw) / 2), b.y + ((b.h - btn_text_h) / 2), label, btn_font,
                           text);
    };

    bool confirm_focused = dialog.focused_button() == ConfirmDialog::Button::Confirm;

    draw_button(lay.cancel, "Cancel",
                Color::from_rgba(1.0, 1.0, 1.0, confirm_focused ? 0.10 : 0.22), theme.foreground,
                !confirm_focused);

    std::string confirm_label = action.name;
    if (dialog.has_countdown()) confirm_label += " · " + std::to_string(dialog.remaining_seconds());
    draw_button(lay.confirm, confirm_label,
                Color::from_rgba(tone.r, tone.g, tone.b, confirm_focused ? 0.92 : 0.45),
                confirm_focused ? Color::from_rgba(0.08, 0.08, 0.10, 0.96) : theme.foreground,
                confirm_focused);
}

} // namespace waylaunch
