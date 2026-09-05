#pragma once

#include "waylaunch/power/confirm_dialog.h"
#include "waylaunch/renderer.h"

namespace waylaunch {

// Draws the macOS-style confirmation card. Its own translation unit (SRP): the
// grid renderer calls it as a collaborator when the dialog is open.
class ConfirmDialogRenderer {
  public:
    ConfirmDialogRenderer() = default;

    static void render(Renderer& renderer, const ConfirmDialog& dialog, const Theme& theme,
                       int screen_w, int screen_h, double font_scale = 1.0);
};

} // namespace waylaunch
