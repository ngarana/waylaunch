#include "waylaunch/switcher/switcher_renderer.h"
#include <algorithm>
#include <cmath>

namespace waylaunch {

void SwitcherRenderer::render(Renderer& renderer,
                               const AppSwitcherManager& manager,
                               const Theme& theme,
                               int screen_w,
                               int screen_h) {
    if (!manager.is_visible()) return;

    const auto& groups = manager.app_groups();
    if (groups.empty()) return;

    // Layout constants matching macOS Command+Tab HUD specs
    constexpr int item_width = 104;
    constexpr int item_height = 104;
    constexpr int icon_size = 64;
    constexpr int padding_x = 24;
    constexpr int padding_y = 20;
    constexpr int corner_radius = 24;
    constexpr int item_radius = 16;
    constexpr int title_margin = 18;

    int num_items = static_cast<int>(groups.size());
    int content_width = num_items * item_width;
    int hud_width = content_width + (padding_x * 2);
    
    // Constraint max width to 80% screen width
    int max_hud_width = static_cast<int>(screen_w * 0.8);
    if (hud_width > max_hud_width) {
        hud_width = max_hud_width;
    }

    int hud_height = item_height + (padding_y * 2);
    int hud_x = (screen_w - hud_width) / 2;
    int hud_y = (screen_h - hud_height) / 2;

    // 1. Draw Frosted Glass Backdrop if available
    if (renderer.has_backdrop()) {
        renderer.draw_backdrop(hud_x, hud_y, hud_width, hud_height, corner_radius);
    }

    // 2. Glass tint background & border
    renderer.rounded_rect(hud_x, hud_y, hud_width, hud_height, corner_radius,
                          Color::from_rgba(0.1, 0.1, 0.14, 0.82));
    renderer.rounded_rect(hud_x, hud_y, hud_width, hud_height, corner_radius,
                          Color::from_rgba(1.0, 1.0, 1.0, 0.12));

    // 3. Render items
    size_t selected_idx = manager.selected_index();
    int start_x = hud_x + padding_x;
    int start_y = hud_y + padding_y;

    for (int i = 0; i < num_items; ++i) {
        int ix = start_x + (i * item_width);
        int iy = start_y;

        // Skip if outside horizontal HUD bounds
        if (ix + item_width > hud_x + hud_width - padding_x) break;

        const auto& grp = groups[i];
        bool is_selected = (static_cast<size_t>(i) == selected_idx);

        // Selection card background pill
        if (is_selected) {
            Color sel_color = Color::from_rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.28);
            renderer.rounded_rect(ix + 4, iy + 4, item_width - 8, item_height - 8, item_radius, sel_color);
            renderer.rounded_rect(ix + 4, iy + 4, item_width - 8, item_height - 8, item_radius,
                                  Color::from_rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.6));
        }

        // Draw Icon
        int icon_x = ix + (item_width - icon_size) / 2;
        int icon_y = iy + (item_height - icon_size) / 2;
        
        std::string label = grp.display_name.empty() ? "?" : grp.display_name.substr(0, 1);
        renderer.draw_icon(icon_x, icon_y, icon_size, grp.icon_name, label, theme.accent);

        // Draw Minimized Indicator Dot if all windows are minimized
        if (grp.is_all_minimized()) {
            renderer.rounded_rect(icon_x + icon_size / 2 - 3, iy + item_height - 12, 6, 6, 3,
                                  Color::from_rgba(0.7, 0.7, 0.7, 0.8));
        }
    }

    // 4. Render App Title centered below the HUD
    const AppGroup* sel_grp = manager.selected_group();
    if (sel_grp) {
        std::string title_text = sel_grp->display_name;
        if (!sel_grp->windows.empty() && !sel_grp->windows.front().title.empty()) {
            title_text += " — " + sel_grp->windows.front().title;
        }

        RenderFontConfig font;
        font.family = theme.result_font.family;
        font.size = 15.0;
        font.bold = true;

        int tw = renderer.text_width(title_text, font);
        int tx = (screen_w - tw) / 2;
        int ty = hud_y + hud_height + title_margin;

        // Title pill background
        int tpill_padding = 12;
        renderer.rounded_rect(tx - tpill_padding, ty - 4, tw + (tpill_padding * 2), 28, 14,
                              Color::from_rgba(0.08, 0.08, 0.12, 0.85));

        renderer.draw_text(tx, ty, title_text, font, theme.foreground);
    }
}

} // namespace waylaunch
