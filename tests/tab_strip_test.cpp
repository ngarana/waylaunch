#include "waylaunch/dropdown/tab_strip.h"

#include <cassert>
#include <iostream>

using namespace waylaunch;

void test_empty() {
    TabStrip strip;
    assert(strip.count() == 0);
    assert(strip.layout(1920).empty());
    assert(strip.hit_test(10, 10, 1920) == 0);
    std::cout << "[PASS] empty\n";
}

void test_equal_layout() {
    TabStrip strip;
    strip.update({{.handle_id = 1, .title = "one", .is_active = false},
                  {.handle_id = 2, .title = "two", .is_active = true}});
    auto rects = strip.layout(1000);
    assert(rects.size() == 2);
    assert(rects[0].x == 0 && rects[0].w == 500 && rects[0].h == TabStrip::kHeight);
    assert(rects[1].x == 500 && rects[1].w == 500);
    // Three tabs divide evenly; the strip spans the full width.
    strip.update({{.handle_id = 1, .title = "a", .is_active = false},
                  {.handle_id = 2, .title = "b", .is_active = false},
                  {.handle_id = 3, .title = "c", .is_active = false}});
    rects = strip.layout(900);
    assert(rects[2].x + rects[2].w == 900);
    std::cout << "[PASS] equal layout\n";
}

void test_hit_test() {
    TabStrip strip;
    strip.update({{.handle_id = 11, .title = "one", .is_active = false},
                  {.handle_id = 22, .title = "two", .is_active = true}});
    assert(strip.hit_test(10, 10, 1000) == 11);
    assert(strip.hit_test(510, 10, 1000) == 22);
    assert(strip.hit_test(999, 35, 1000) == 22);
    assert(strip.hit_test(1000, 10, 1000) == 0); // past the edge
    assert(strip.hit_test(10, -1, 1000) == 0);   // above the strip
    assert(strip.hit_test(10, 36, 1000) == 0);   // below the strip
    assert(strip.hit_test(-5, 10, 1000) == 0);   // left of the strip
    std::cout << "[PASS] hit test\n";
}

void test_update_replaces() {
    TabStrip strip;
    strip.update({{.handle_id = 1, .title = "one", .is_active = false}});
    assert(strip.count() == 1);
    strip.update({{.handle_id = 2, .title = "two", .is_active = true},
                  {.handle_id = 3, .title = "three", .is_active = false}});
    assert(strip.count() == 2);
    assert(strip.hit_test(10, 10, 1000) == 2); // stale tab one is gone
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
