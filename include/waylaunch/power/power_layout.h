#pragma once

#include <vector>

namespace waylaunch {

// Pure geometry for the power overlay — the single source of truth shared by
// the renderers (where to draw) and pointer hit-testing (what's under the
// cursor), mirroring how LauncherUI::relayout() feeds both render and hit_test.
// No cairo, no Wayland: header-friendly and unit-testable.
namespace power_layout {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// The frosted HUD: its panel plus one cell rect per action (in action order;
// only cards that fit within the panel are included, matching the renderer's
// break). icon_size lets the renderer place the glyph/label from the cell.
struct Hud {
    Rect panel;
    std::vector<Rect> cards;
    int corner_radius = 24;
    int icon_size = 64;
    int icon_gap_top = 10;   // gap from cell top to the icon
};
Hud hud(int screen_w, int screen_h, int num_actions);

// The confirmation card and its two pill buttons.
struct Dialog {
    Rect card;
    Rect cancel;
    Rect confirm;
    int corner_radius = 28;
    int pad = 28;
};
Dialog dialog(int screen_w, int screen_h);

} // namespace power_layout
} // namespace waylaunch
