#include "waylaunch/power/power_layout.h"
#include <cassert>
#include <iostream>

using namespace waylaunch::power_layout;

void test_hud_centered_and_ordered() {
    Hud h = hud(1280, 800, 6);
    assert(h.cards.size() == 6);

    // Panel centered on screen.
    assert(h.panel.x + h.panel.w / 2 == 640);
    assert(h.panel.y + h.panel.h / 2 == 400);

    // Cards are inside the panel, in order, non-overlapping, uniform width.
    int prev_right = h.panel.x;
    int w0 = h.cards[0].w;
    for (const auto& c : h.cards) {
        assert(c.w == w0);
        assert(c.x >= h.panel.x && c.x + c.w <= h.panel.x + h.panel.w);
        assert(c.x >= prev_right);
        prev_right = c.x + c.w;
        assert(c.y >= h.panel.y && c.y + c.h <= h.panel.y + h.panel.h);
    }
    std::cout << "[PASS] hud centered and ordered\n";
}

void test_hud_contains_matches_card() {
    Hud h = hud(1280, 800, 6);
    const auto& c = h.cards[3];
    assert(c.contains(c.x + c.w / 2.0, c.y + c.h / 2.0));   // center hits
    assert(!c.contains(c.x - 1, c.y));                      // just left: miss
    assert(!c.contains(c.x + c.w, c.y));                    // right edge exclusive
    // The gap between two cards belongs to neither (cells are exact, no overlap).
    std::cout << "[PASS] hud contains matches card\n";
}

void test_hud_empty() {
    Hud h = hud(1280, 800, 0);
    assert(h.cards.empty());
    assert(h.panel.w == 0 && h.panel.h == 0);
    std::cout << "[PASS] hud empty\n";
}

void test_hud_clamps_wide() {
    // Absurd action count: panel clamps to 80% and only fitting cards are kept.
    Hud h = hud(1000, 800, 40);
    assert(h.panel.w <= 800);   // 80% of 1000
    assert(h.cards.size() < 40);
    for (const auto& c : h.cards)
        assert(c.x + c.w <= h.panel.x + h.panel.w);
    std::cout << "[PASS] hud clamps wide\n";
}

void test_dialog_buttons_inside_card() {
    Dialog d = dialog(1280, 800);
    assert(d.card.x + d.card.w / 2 == 640);
    assert(d.card.y + d.card.h / 2 == 400);

    for (const auto* b : {&d.cancel, &d.confirm}) {
        assert(b->x >= d.card.x && b->x + b->w <= d.card.x + d.card.w);
        assert(b->y >= d.card.y && b->y + b->h <= d.card.y + d.card.h);
    }
    // Cancel is left of Confirm and they don't overlap.
    assert(d.cancel.x + d.cancel.w <= d.confirm.x);
    assert(d.cancel.w == d.confirm.w);
    // Button centers hit their own rect and not the other.
    assert(d.cancel.contains(d.cancel.x + d.cancel.w / 2.0, d.cancel.y + d.cancel.h / 2.0));
    assert(!d.confirm.contains(d.cancel.x + d.cancel.w / 2.0, d.cancel.y + d.cancel.h / 2.0));
    std::cout << "[PASS] dialog buttons inside card\n";
}

int main() {
    test_hud_centered_and_ordered();
    test_hud_contains_matches_card();
    test_hud_empty();
    test_hud_clamps_wide();
    test_dialog_buttons_inside_card();
    std::cout << "All power layout tests passed!\n";
    return 0;
}
