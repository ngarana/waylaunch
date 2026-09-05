#include "waylaunch/renderer.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {
uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace

int main() {
    constexpr int width = 320;
    constexpr int height = 180;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);

    waylaunch::Renderer renderer;
    renderer.begin(pixels.data(), stride, width, height);
    renderer.clear(waylaunch::Color::from_rgba(0.08, 0.08, 0.12, 1.0));
    renderer.rounded_rect(18, 22, 284, 112, 16,
                          waylaunch::Color::from_rgba(0.12, 0.15, 0.22, 0.96));
    renderer.fill_rect(36, 56, 248, 1, waylaunch::Color::from_rgba(0.54, 0.71, 0.98, 0.55));
    renderer.draw_search_glyph(50, 76, 24, waylaunch::Color::from_rgba(0.54, 0.71, 0.98, 0.95));
    renderer.end();

    constexpr uint64_t expected = 13195369424614640600ULL;
    const uint64_t actual = fnv1a(pixels);
    if (actual != expected) {
        std::cerr << "renderer golden hash: " << actual << "\n";
        return 1;
    }
    return 0;
}
