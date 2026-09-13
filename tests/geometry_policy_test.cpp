#include "waylaunch/dropdown/geometry_policy.h"

#include <cassert>
#include <iostream>

using namespace waylaunch;

MonitorInfo test_monitor() {
    MonitorInfo monitor;
    monitor.name = "eDP-1";
    monitor.x = 0;
    monitor.y = 0;
    monitor.w = 1920;
    monitor.h = 1200;
    monitor.scale = 1.0;
    monitor.reserved_top = 44; // bar inset
    monitor.reserved_bottom = 0;
    monitor.active_workspace = 2;
    return monitor;
}

void test_top_default_sits_below_bar() {
    Geometry geom = compute_geometry(test_monitor(), DropdownConfig{});
    assert(geom.x == 0);
    assert(geom.y == 44);
    assert(geom.w == 1920); // 100% of 1920
    assert(geom.h == 462);  // 40% of (1200 - 44) = 462.4 → 462
    std::cout << "[PASS] top default sits below bar\n";
}

void test_bottom_anchors_to_usable_base() {
    DropdownConfig config;
    config.edge = DropdownEdge::Bottom;
    Geometry geom = compute_geometry(test_monitor(), config);
    assert(geom.y + geom.h == 1200);
    assert(geom.h == 462);
    std::cout << "[PASS] bottom anchors to usable base\n";
}

void test_side_edges_anchor_horizontally() {
    DropdownConfig left;
    left.edge = DropdownEdge::Left;
    left.width_percent = 50;
    Geometry l = compute_geometry(test_monitor(), left);
    // Vertical is the free axis for a side edge, so it centres: 40% of 1156
    // is 462, leaving 347 above.
    assert(l.x == 0 && l.y == 44 + 347 && l.w == 960);

    DropdownConfig right;
    right.edge = DropdownEdge::Right;
    right.width_percent = 50;
    Geometry r = compute_geometry(test_monitor(), right);
    assert(r.x + r.w == 1920 && r.w == 960);
    std::cout << "[PASS] side edges anchor horizontally\n";
}

void test_partial_width_centres_on_the_free_axis() {
    DropdownConfig config;
    config.width_percent = 67;
    config.height_percent = 32;
    Geometry geom = compute_geometry(test_monitor(), config);
    assert(geom.w == 1286);              // 67% of 1920 = 1286.4 → 1286
    assert(geom.h == 369);               // 32% of 1156 = 369.9 → 369
    assert(geom.x == (1920 - 1286) / 2); // centred, not flush left
    assert(geom.y == 44);                // still pinned under the bar
    std::cout << "[PASS] partial width centres on the free axis\n";
}

void test_percents_clamp() {
    DropdownConfig config;
    config.width_percent = 1000;
    config.height_percent = -5;
    Geometry geom = compute_geometry(test_monitor(), config);
    assert(geom.w == 1920); // clamped to 100%
    assert(geom.h == 11);   // clamped to 1% of 1156
    std::cout << "[PASS] percents clamp\n";
}

void test_reserved_bottom_shrinks_usable_area() {
    MonitorInfo monitor = test_monitor();
    monitor.reserved_bottom = 56;
    Geometry geom = compute_geometry(monitor, DropdownConfig{});
    assert(geom.y == 44);
    assert(geom.h == 440); // 40% of (1200 - 44 - 56) = 440
    DropdownConfig bottom;
    bottom.edge = DropdownEdge::Bottom;
    Geometry b = compute_geometry(monitor, bottom);
    assert(b.y + b.h == 1200 - 56);
    std::cout << "[PASS] reserved bottom shrinks usable area\n";
}

void test_size_override_replaces_computed_size() {
    DropdownConfig config;
    config.size_override = Geometry{.x = 0, .y = 0, .w = 800, .h = 600};
    Geometry geom = compute_geometry(test_monitor(), config);
    assert(geom.w == 800 && geom.h == 600);
    // Placement still follows the edge: pinned under the bar, centred across.
    assert(geom.x == (1920 - 800) / 2 && geom.y == 44);
    std::cout << "[PASS] size override replaces computed size\n";
}

void test_multi_monitor_offset() {
    MonitorInfo monitor = test_monitor();
    monitor.x = 1920;
    monitor.y = 100;
    Geometry geom = compute_geometry(monitor, DropdownConfig{});
    assert(geom.x == 1920 && geom.y == 144);
    std::cout << "[PASS] multi monitor offset\n";
}

void test_resolve_slot() {
    DropdownConfig global;
    global.height_percent = 40;
    // No slots configured: ad-hoc names yield section defaults untouched.
    auto implicit = resolve_dropdown_slot(global, "scratch");
    assert(implicit.command.empty());
    assert(implicit.config.height_percent == 40);
    // Named slot: command plus present overrides only (-1 inherits).
    global.slots.push_back(
        {.name = "notes", .command = "kitty -e nvim", .width_percent = -1, .height_percent = 60});
    auto notes = resolve_dropdown_slot(global, "notes");
    assert(notes.command == "kitty -e nvim");
    assert(notes.config.height_percent == 60);
    assert(notes.config.width_percent == global.width_percent);
    auto other = resolve_dropdown_slot(global, "term");
    assert(other.command.empty() && other.config.height_percent == 40);
    std::cout << "[PASS] resolve slot\n";
}

// Gap-5 remainder: a manual resize is learned on hide, jitter is not.
void test_learn_resize() {
    Geometry placed{.x = 0, .y = 36, .w = 1920, .h = 444};

    // Untouched: exactly what we placed.
    assert(!learn_resize(placed, placed, 36, 8, true).has_value());

    // Compositor rounding stays under the epsilon.
    Geometry jitter{.x = 0, .y = 36, .w = 1918, .h = 441};
    assert(!learn_resize(placed, jitter, 36, 8, true).has_value());

    // A real drag is learned, with the strip band added back so the stored
    // size describes the whole dropdown.
    Geometry dragged{.x = 0, .y = 36, .w = 1920, .h = 700};
    auto learned = learn_resize(placed, dragged, 36, 8, true);
    assert(learned.has_value());
    assert(learned->w == 1920);
    assert(learned->h == 736);

    // With no strip the terminal is the whole dropdown.
    auto no_strip = learn_resize(placed, dragged, 0, 8, true);
    assert(no_strip.has_value() && no_strip->h == 700);

    // Width-only changes count too.
    Geometry narrowed{.x = 0, .y = 36, .w = 1200, .h = 444};
    assert(learn_resize(placed, narrowed, 36, 8, true).has_value());

    // Degenerate readings are ignored rather than persisted.
    assert(
        !learn_resize(placed, Geometry{.x = 0, .y = 0, .w = 0, .h = 0}, 36, 8, true).has_value());
    assert(!learn_resize(Geometry{}, dragged, 36, 8, true).has_value());

    // A tiled (or otherwise externally reshaped) window is the compositor's
    // doing, never a manual resize — learning it would pin layout geometry
    // over the configured size on every future show.
    Geometry tiled{.x = 16, .y = 85, .w = 1898, .h = 1145};
    assert(!learn_resize(placed, tiled, 36, 8, false).has_value());

    // A learned size round-trips through the policy as the override.
    DropdownConfig config;
    config.size_override = learned;
    MonitorInfo monitor{.name = "eDP-1", .x = 0, .y = 0, .w = 1920, .h = 1200, .scale = 1.0};
    Geometry applied = compute_geometry(monitor, config);
    assert(applied.w == 1920 && applied.h == 736);

    std::cout << "[PASS] learn resize\n";
}

int main() {
    test_learn_resize();
    test_top_default_sits_below_bar();
    test_bottom_anchors_to_usable_base();
    test_side_edges_anchor_horizontally();
    test_partial_width_centres_on_the_free_axis();
    test_percents_clamp();
    test_reserved_bottom_shrinks_usable_area();
    test_size_override_replaces_computed_size();
    test_multi_monitor_offset();
    test_resolve_slot();
    std::cout << "geometry_policy_test: all passed\n";
    return 0;
}
