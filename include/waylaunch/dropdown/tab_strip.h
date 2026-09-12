#pragma once

#include "waylaunch/renderer.h"

#include <string>
#include <vector>

namespace waylaunch {

// Phase 5 owned tab strip (docs/DROPDOWN_IMPLEMENTATION.md §5): a thin layer
// surface rendered above the terminal, listing the slot's windows as tabs.
// `keyboard_interactivity: NONE`, so it takes pointer clicks without stealing
// the keyboard. Layout and hit-testing are pure (unit-tested); render reuses
// the existing text-and-rects Renderer, which is exactly what a dozen tabs
// need.
//
// Tabs are keyed by compositor address and sourced from `j/clients`, the same
// place placement reads from. That is deliberate: it makes tab membership
// obey the slot's pid-ownership rule (so a hand-spawned same-class window is
// not a tab), and it drops the foreign-toplevel dependency the strip used to
// carry — the protocol reports no pid, so it could not tell the two apart.
// Switching is IPlacementBackend::focus, and window open/close/title/focus
// events already arrive on the event stream the daemon polls.
class TabStrip {
  public:
    static constexpr int kHeight = 36;

    struct Tab {
        std::string address;
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
    // Address under (x, y), or empty for a miss (including empty strip).
    std::string hit_test(int x, int y, int total_width) const;

    void render(Renderer& renderer, int total_width, const Colors& colors,
                const RenderFontConfig& font) const;

  private:
    std::vector<Tab> tabs_;
};

} // namespace waylaunch
