#pragma once

#include "waylaunch/renderer.h"
#include "waylaunch/switcher/app_switcher_manager.h"

namespace waylaunch {

class SwitcherRenderer {
public:
    SwitcherRenderer() = default;

    void render(Renderer& renderer,
                const AppSwitcherManager& manager,
                const Theme& theme,
                int screen_w,
                int screen_h);
};

} // namespace waylaunch
