#pragma once

#include "waylaunch/renderer.h"
#include <string>

namespace waylaunch {

// Hand-drawn vector glyphs for the six power actions — crisp at any size and
// identical on every system, instead of whatever the icon theme happens to
// ship. Drawn with the renderer's active cairo context (Renderer::cr()).
namespace power_glyphs {

// Draw the glyph for `action_id` centered at (cx, cy) in a `size` box.
// Unknown ids fall back to the IEC power symbol.
void draw(Renderer& renderer, const std::string& action_id,
          double cx, double cy, double size, const Color& color);

} // namespace power_glyphs

} // namespace waylaunch
