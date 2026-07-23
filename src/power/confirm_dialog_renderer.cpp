#include "waylaunch/power/confirm_dialog_renderer.h"
#include "waylaunch/power/power_glyphs.h"
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

void ConfirmDialogRenderer::render(Renderer& renderer,
                                   const ConfirmDialog& dialog,
                                   const Theme& theme,
                                   int screen_w,
                                   int screen_h,
                                   double font_scale) {
    if (!dialog.is_open()) return;
    const PowerAction& action = dialog.action();

    constexpr int card_w = 420;
    constexpr int card_h = 316;
    constexpr int corner_radius = 28;   // "mostly round" — softer than the HUD
    constexpr int pad = 28;

    int x = (screen_w - card_w) / 2;
    int y = (screen_h - card_h) / 2;

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
    renderer.fill_rect(x + corner_radius, y, card_w - 2 * corner_radius, 1,
                       Color::from_rgba(1.0, 1.0, 1.0, 0.22));

    int inner_x = x + pad;
    int inner_w = card_w - 2 * pad;

    // Session-ending actions are red; hibernate/suspend use the accent.
    const Color& tone = action.subtext.empty() ? theme.accent : theme.error;

    // --- Round action badge with the glyph, ringed by the countdown ---
    double icx = x + card_w / 2.0;
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
            cairo_arc(cr, icx, icy, ring_r, -M_PI_2, -M_PI_2 + 2 * M_PI * frac);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    // --- Centered headline + optional subtext ---
    RenderFontConfig headline = theme.result_font;
    headline.size = 17.0 * font_scale;
    headline.bold = true;
    int cy = y + 64 + badge_r + 22;
    cy += renderer.draw_markup(inner_x, cy, escape_markup(action.confirm_text),
                               headline, theme.foreground, inner_w, 2, true) + 8;

    if (!action.subtext.empty()) {
        RenderFontConfig body = theme.result_detail_font;
        body.size *= font_scale;
        renderer.draw_markup(inner_x, cy, escape_markup(action.subtext), body,
                             theme.text_muted, inner_w, 2, true);
    }

    // --- Fully-round pill buttons; the focused one is filled, the other quiet.
    //     ←/→/Tab move focus, Return/Space press the focused button. The counter
    //     lives in the confirm label ("Shut Down · 42"), language-neutral. ---
    constexpr int btn_h = 44;
    constexpr int btn_gap = 12;
    int btn_w = (inner_w - btn_gap) / 2;
    int by = y + card_h - pad - btn_h;

    RenderFontConfig btn_font = theme.result_font;
    btn_font.size = 14.0 * font_scale;
    btn_font.bold = true;
    int btn_text_h = renderer.text_height(btn_font);

    auto draw_button = [&](int bx, const std::string& label, const Color& fill,
                           const Color& text, bool focused) {
        if (focused) {   // 2px halo ring behind the pill
            renderer.rounded_rect(bx - 3, by - 3, btn_w + 6, btn_h + 6, (btn_h + 6) / 2,
                                  Color::from_rgba(1.0, 1.0, 1.0, 0.30));
        }
        renderer.rounded_rect(bx, by, btn_w, btn_h, btn_h / 2, fill);
        int tw = renderer.text_width(label, btn_font);
        renderer.draw_text(bx + (btn_w - tw) / 2, by + (btn_h - btn_text_h) / 2,
                           label, btn_font, text);
    };

    bool confirm_focused =
        dialog.focused_button() == ConfirmDialog::Button::Confirm;

    draw_button(inner_x, "Cancel",
                Color::from_rgba(1.0, 1.0, 1.0, confirm_focused ? 0.10 : 0.22),
                theme.foreground, !confirm_focused);

    std::string confirm_label = action.name;
    if (dialog.has_countdown())
        confirm_label += " · " + std::to_string(dialog.remaining_seconds());
    draw_button(inner_x + btn_w + btn_gap, confirm_label,
                Color::from_rgba(tone.r, tone.g, tone.b, confirm_focused ? 0.92 : 0.45),
                confirm_focused ? Color::from_rgba(0.08, 0.08, 0.10, 0.96)
                                : theme.foreground,
                confirm_focused);
}

} // namespace waylaunch
