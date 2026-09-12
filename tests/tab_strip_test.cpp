#include "waylaunch/dropdown/tab_strip.h"

#include <cassert>
#include <iostream>

using namespace waylaunch;

void test_empty() {
    TabStrip strip;
    assert(strip.count() == 0);
    assert(strip.layout(1920).empty());
    assert(strip.hit_test(10, 10, 1920).empty());
    std::cout << "[PASS] empty\n";
}

void test_equal_layout() {
    TabStrip strip;
    strip.update({{.address = "0x1", .title = "one", .is_active = false},
                  {.address = "0x2", .title = "two", .is_active = true}});
    auto rects = strip.layout(1000);
    assert(rects.size() == 2);
    assert(rects[0].x == 0 && rects[0].w == 500 && rects[0].h == TabStrip::kHeight);
    assert(rects[1].x == 500 && rects[1].w == 500);
    // Three tabs divide evenly; the strip spans the full width.
    strip.update({{.address = "0x1", .title = "a", .is_active = false},
                  {.address = "0x2", .title = "b", .is_active = false},
                  {.address = "0x3", .title = "c", .is_active = false}});
    rects = strip.layout(900);
    assert(rects[2].x + rects[2].w == 900);
    std::cout << "[PASS] equal layout\n";
}

void test_hit_test() {
    TabStrip strip;
    strip.update({{.address = "0x11", .title = "one", .is_active = false},
                  {.address = "0x22", .title = "two", .is_active = true}});
    assert(strip.hit_test(10, 10, 1000) == "0x11");
    assert(strip.hit_test(510, 10, 1000) == "0x22");
    assert(strip.hit_test(999, 35, 1000) == "0x22");
    assert(strip.hit_test(1000, 10, 1000).empty()); // past the edge
    assert(strip.hit_test(10, -1, 1000).empty());   // above the strip
    assert(strip.hit_test(10, 36, 1000).empty());   // below the strip
    assert(strip.hit_test(-5, 10, 1000).empty());   // left of the strip
    std::cout << "[PASS] hit test\n";
}

void test_update_replaces() {
    TabStrip strip;
    strip.update({{.address = "0x1", .title = "one", .is_active = false}});
    assert(strip.count() == 1);
    strip.update({{.address = "0x2", .title = "two", .is_active = true},
                  {.address = "0x3", .title = "three", .is_active = false}});
    assert(strip.count() == 2);
    assert(strip.hit_test(10, 10, 1000) == "0x2"); // stale tab one is gone
    std::cout << "[PASS] update replaces\n";
}

int main() {
    test_empty();
    test_equal_layout();
    test_hit_test();
    test_update_replaces();
    std::cout << "tab_strip_test: all passed\n";
    return 0;
}
