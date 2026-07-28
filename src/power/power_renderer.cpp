#include "waylaunch/power/power_renderer.h"
#include "waylaunch/power/power_glyphs.h"
#include "waylaunch/power/power_layout.h"
#include <algorithm>

namespace waylaunch {

void PowerRenderer::render(Renderer& renderer,
                           const PowerManager& manager,
                           const Theme& theme,
                           int screen_w,
                           int screen_h,
                           double font_scale) {
    if (!manager.is_visible()) return;

    const auto& actions = manager.actions();
    if (actions.empty()) return;

    // Confirming replaces the HUD rather than stacking on it: the two cards are
    // both centered, so layering left the HUD's ends poking out either side of
    // the dialog — two competing floating panels. Once you have committed to an
    // action the picker is done, so the dialog stands alone (and its own glyph
    // badge says which action you are confirming).
    if (manager.confirm_dialog().is_open()) {
        dialog_renderer_.render(renderer, manager.confirm_dialog(), theme,
                                screen_w, screen_h, font_scale);
        return;
    }

    // Geometry from the shared layout (single source of truth with hit-testing).
    constexpr int item_radius = 16;
    auto lay = power_layout::hud(screen_w, screen_h, static_cast<int>(actions.size()));
    const auto& panel = lay.panel;
    const int icon_size = lay.icon_size;

    // 1. Frosted glass backdrop + glass tint + border (switcher treatment).
    if (renderer.has_backdrop()) {
        renderer.draw_backdrop(panel.x, panel.y, panel.w, panel.h, lay.corner_radius);
    }
    renderer.rounded_rect(panel.x, panel.y, panel.w, panel.h, lay.corner_radius,
                          Color::from_rgba(0.1, 0.1, 0.14, 0.82));
    renderer.rounded_rect(panel.x, panel.y, panel.w, panel.h, lay.corner_radius,
                          Color::from_rgba(1.0, 1.0, 1.0, 0.12));

    // 2. Action cards: icon on top, subtle label underneath.
    size_t selected_idx = manager.selected_index();

    RenderFontConfig label_font = theme.result_font;
    label_font.size = std::max(9.0, 11.0 * font_scale);

    for (size_t i = 0; i < lay.cards.size(); ++i) {
        const auto& cell = lay.cards[i];
        int ix = cell.x, iy = cell.y;

        const auto& action = actions[i];
        bool is_selected = (i == selected_idx);
        if (is_selected) {
            renderer.draw_selection_pill(ix + 4, iy + 4, cell.w - 8, cell.h - 8,
                                         item_radius, theme.accent);
        }

        // Round icon button with a hand-drawn glyph — consistent everywhere,
        // independent of the installed icon theme. Shut Down is softly
        // red-tinted; everything else stays neutral (macOS restraint).
        double cx = ix + cell.w / 2.0;
        double cy = iy + lay.icon_gap_top + icon_size / 2.0;
        int cr_r = icon_size / 2;
        bool is_shutdown = action.id == "shutdown";
        Color circle = is_shutdown
            ? Color::from_rgba(theme.error.r, theme.error.g, theme.error.b, 0.22)
            : Color::from_rgba(1.0, 1.0, 1.0, 0.12);
        renderer.rounded_rect(static_cast<int>(cx) - cr_r, static_cast<int>(cy) - cr_r,
                              icon_size, icon_size, cr_r, circle);
        Color glyph = is_shutdown
            ? Color::from_rgba(theme.error.r, theme.error.g, theme.error.b, 0.95)
            : Color::from_rgba(theme.foreground.r, theme.foreground.g,
                               theme.foreground.b, 0.92);
        power_glyphs::draw(renderer, action.id, cx, cy, icon_size * 0.66, glyph);

        // The label is the only name this action gets, so the selected one goes
        // full-strength — carrying the emphasis the removed title pill used to.
        RenderFontConfig lf = label_font;
        lf.bold = is_selected;
        int lw = renderer.text_width(action.name, lf);
        renderer.draw_text(ix + (cell.w - std::min(lw, cell.w - 8)) / 2,
                           iy + lay.icon_gap_top + icon_size + 6, action.name, lf,
                           Color::from_rgba(theme.foreground.r, theme.foreground.g,
                                            theme.foreground.b, is_selected ? 1.0 : 0.78));
    }

    // No title pill below the HUD: unlike the switcher — whose cards are bare
    // icons — every action card is self-labelled, so a pill naming the
    // selection would only repeat the label already under the highlighted card.
}

} // namespace waylaunch
