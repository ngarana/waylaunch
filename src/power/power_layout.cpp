#include <algorithm>

#include "waylaunch/power/power_layout.h"

namespace waylaunch::power_layout {

Hud hud(int screen_w, int screen_h, int num_actions) {
    // Same constants the switcher HUD uses (keep in lockstep with PowerRenderer).
    constexpr int item_width = 104;
    constexpr int item_height = 104;
    constexpr int icon_size = 64;
    constexpr int padding_x = 24;
    constexpr int padding_y = 20;
    constexpr int corner_radius = 24;

    Hud out;
    out.corner_radius = corner_radius;
    out.icon_size = icon_size;
    if (num_actions <= 0) return out;

    int hud_width = (num_actions * item_width) + (padding_x * 2);
    int max_hud_width = static_cast<int>(screen_w * 0.8);
    hud_width = std::min(hud_width, max_hud_width);

    int hud_height = item_height + (padding_y * 2);
    int hud_x = (screen_w - hud_width) / 2;
    int hud_y = (screen_h - hud_height) / 2;
    out.panel = {.x = hud_x, .y = hud_y, .w = hud_width, .h = hud_height};

    int start_x = hud_x + padding_x;
    int start_y = hud_y + padding_y;
    for (int i = 0; i < num_actions; ++i) {
        int ix = start_x + (i * item_width);
        if (ix + item_width > hud_x + hud_width - padding_x) break; // clipped: drop it
        out.cards.push_back({.x = ix, .y = start_y, .w = item_width, .h = item_height});
    }
    return out;
}

Dialog dialog(int screen_w, int screen_h) {
    constexpr int card_w = 420;
    constexpr int card_h = 316;
    constexpr int corner_radius = 28;
    constexpr int pad = 28;
    constexpr int btn_h = 44;
    constexpr int btn_gap = 12;

    Dialog out;
    out.corner_radius = corner_radius;
    out.pad = pad;

    int x = (screen_w - card_w) / 2;
    int y = (screen_h - card_h) / 2;
    out.card = {.x = x, .y = y, .w = card_w, .h = card_h};

    int inner_x = x + pad;
    int inner_w = card_w - (2 * pad);
    int btn_w = (inner_w - btn_gap) / 2;
    int by = y + card_h - pad - btn_h;
    out.cancel = {.x = inner_x, .y = by, .w = btn_w, .h = btn_h};
    out.confirm = {.x = inner_x + btn_w + btn_gap, .y = by, .w = btn_w, .h = btn_h};
    return out;
}

} // namespace waylaunch::power_layout
