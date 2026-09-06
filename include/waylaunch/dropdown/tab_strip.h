#pragma once

#include "waylaunch/renderer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace waylaunch {

// Phase 5 owned tab strip (docs/DROPDOWN_IMPLEMENTATION.md §5): a thin layer
// surface rendered above the terminal, listing the slot's windows as tabs.
// `keyboard_interactivity: NONE`, so it takes pointer clicks without stealing
// the keyboard. Layout and hit-testing are pure (unit-tested); render reuses
// the existing text-and-rects Renderer, which is exactly what a dozen tabs
// need. Tab switching itself goes through IToplevelBackend::activate, already
// used by the switcher.
class TabStrip {
  public:
    static constexpr int kHeight = 36;

    struct Tab {
        uintptr_t handle_id = 0;
        std::string title;
        bool is_active = false;
    };

    struct Rect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool contains(int px, int py) const {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    struct Colors {
        Color background;
        Color foreground; // inactive tab text
        Color accent;     // active tab pill (active text reuses background)
    };

    void update(std::vector<Tab> tabs);
    size_t count() const { return tabs_.size(); }

    // Equal-width tabs spanning [0, total_width). Empty when no tabs.
    std::vector<Rect> layout(int total_width) const;
    // Handle id under (x, y), or 0 for a miss (including empty strip).
    uintptr_t hit_test(int x, int y, int total_width) const;

    void render(Renderer& renderer, int total_width, const Colors& colors,
                const RenderFontConfig& font) const;

  private:
    std::vector<Tab> tabs_;
};

} // namespace waylaunch
