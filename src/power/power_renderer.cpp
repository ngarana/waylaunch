#include "waylaunch/power/power_renderer.h"
#include "waylaunch/power/power_glyphs.h"
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

    // Same HUD geometry as SwitcherRenderer: a compact frosted card centered on
    // screen — the overlay is switcher-like, not full-screen.
    constexpr int item_width = 104;
    constexpr int item_height = 104;
    constexpr int icon_size = 64;
    constexpr int padding_x = 24;
    constexpr int padding_y = 20;
    constexpr int corner_radius = 24;
    constexpr int item_radius = 16;
    constexpr int title_margin = 18;

    int num_items = static_cast<int>(actions.size());
    int hud_width = num_items * item_width + padding_x * 2;
    int max_hud_width = static_cast<int>(screen_w * 0.8);
    if (hud_width > max_hud_width) hud_width = max_hud_width;

    int hud_height = item_height + padding_y * 2;
    int hud_x = (screen_w - hud_width) / 2;
    int hud_y = (screen_h - hud_height) / 2;

    // 1. Frosted glass backdrop + glass tint + border (switcher treatment).
    if (renderer.has_backdrop()) {
        renderer.draw_backdrop(hud_x, hud_y, hud_width, hud_height, corner_radius);
    }
    renderer.rounded_rect(hud_x, hud_y, hud_width, hud_height, corner_radius,
                          Color::from_rgba(0.1, 0.1, 0.14, 0.82));
    renderer.rounded_rect(hud_x, hud_y, hud_width, hud_height, corner_radius,
                          Color::from_rgba(1.0, 1.0, 1.0, 0.12));

    // 2. Action cards: icon on top, subtle label underneath.
    size_t selected_idx = manager.selected_index();
    int start_x = hud_x + padding_x;
    int start_y = hud_y + padding_y;

    RenderFontConfig label_font = theme.result_font;
    label_font.size = std::max(9.0, 11.0 * font_scale);

    for (int i = 0; i < num_items; ++i) {
        int ix = start_x + (i * item_width);
        int iy = start_y;
        if (ix + item_width > hud_x + hud_width - padding_x) break;

        const auto& action = actions[i];
        if (static_cast<size_t>(i) == selected_idx) {
            renderer.draw_selection_pill(ix + 4, iy + 4, item_width - 8, item_height - 8,
                                         item_radius, theme.accent);
        }

        // Round icon button with a hand-drawn glyph — consistent everywhere,
        // independent of the installed icon theme. Shut Down is softly
        // red-tinted; everything else stays neutral (macOS restraint).
        double cx = ix + item_width / 2.0;
        double cy = iy + 10 + icon_size / 2.0;
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

        int lw = renderer.text_width(action.name, label_font);
        renderer.draw_text(ix + (item_width - std::min(lw, item_width - 8)) / 2,
                           iy + 10 + icon_size + 6, action.name, label_font,
                           Color::from_rgba(theme.foreground.r, theme.foreground.g,
                                            theme.foreground.b, 0.85));
    }

    // 3. Selected action name below the HUD (the switcher's title pill).
    const PowerAction* sel = manager.selected_action();
    if (sel) {
        RenderFontConfig font;
        font.family = theme.result_font.family;
        font.size = 15.0 * font_scale;
        font.bold = true;

        int tw = renderer.text_width(sel->name, font);
        int tx = (screen_w - tw) / 2;
        int ty = hud_y + hud_height + title_margin;

        int tpill_padding = 12;
        renderer.rounded_rect(tx - tpill_padding, ty - 4, tw + (tpill_padding * 2), 28, 14,
                              Color::from_rgba(0.08, 0.08, 0.12, 0.85));
        renderer.draw_text(tx, ty, sel->name, font, theme.foreground);
    }

    // 4. Modal confirmation: dim the grid behind it, then draw the card.
    if (manager.confirm_dialog().is_open()) {
        renderer.rounded_rect(hud_x, hud_y, hud_width, hud_height, corner_radius,
                              Color::from_rgba(0.0, 0.0, 0.0, 0.45));
        dialog_renderer_.render(renderer, manager.confirm_dialog(), theme,
                                screen_w, screen_h, font_scale);
    }
}

} // namespace waylaunch
