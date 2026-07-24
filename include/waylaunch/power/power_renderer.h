#pragma once

#include "waylaunch/renderer.h"
#include "waylaunch/power/power_manager.h"
#include "waylaunch/power/confirm_dialog_renderer.h"

namespace waylaunch {

// Draws the power HUD — the same compact frosted-glass card as the switcher
// (not full-screen): one row of self-labelled action cards, no title pill
// beneath (the switcher needs one because its cards are bare icons; these
// name themselves). Delegates the confirmation dialog to ConfirmDialogRenderer.
// Pure output: reads a PowerManager, never mutates it.
class PowerRenderer {
public:
    PowerRenderer() = default;

    void render(Renderer& renderer,
                const PowerManager& manager,
                const Theme& theme,
                int screen_w,
                int screen_h,
                double font_scale = 1.0);

private:
    ConfirmDialogRenderer dialog_renderer_;
};

} // namespace waylaunch
