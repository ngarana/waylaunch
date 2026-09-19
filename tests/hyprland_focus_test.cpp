// Exact-address workspace follow: the title/class regex trap must not recur.
// A window titled "(17) WhatsApp - Helium" is unmatchable via Hyprland
// `title:` selectors (parens are regex groups); pick_hypr_address() uses exact
// C++ equality, and focus goes to `address:0x...` (hex, no metacharacters).
#include "waylaunch/switcher/hyprland_focus.h"

#include <cassert>
#include <iostream>

namespace {

using waylaunch::HyprClient;

HyprClient make_client(const std::string& address, const std::string& klass,
                       const std::string& title) {
    HyprClient c;
    c.address = address;
    c.klass = klass;
    c.title = title;
    return c;
}

void test_exact_match_with_regex_metachars_in_title() {
    using namespace waylaunch;
    // Mirrors the live repro: workspace 1 holds Helium + an XWayland sheet.
    std::vector<HyprClient> clients = {
        make_client("0x5620c9dc0980", "et", "pending_dataset2_REVISED_UPDATED (2).csv"),
        make_client("0x5620c90633e0", "helium", "(17) WhatsApp - Helium"),
    };
    auto got = pick_hypr_address(clients, "helium", "(17) WhatsApp - Helium");
    assert(got.has_value());
    assert(*got == "0x5620c90633e0");
    std::cout << "[PASS] exact class+title with regex metachars\n";
}

void test_title_only_fallback_covers_class_skew() {
    using namespace waylaunch;
    // XWayland: wlr app_id and Hyprland class disagree; title still identifies.
    std::vector<HyprClient> clients = {
        make_client("0xabc1", "ActualClass", "(17) WhatsApp - Helium"),
    };
    auto got = pick_hypr_address(clients, "wlr-reported-id", "(17) WhatsApp - Helium");
    assert(got.has_value());
    assert(*got == "0xabc1");
    std::cout << "[PASS] title-only fallback\n";
}

void test_class_only_singleton() {
    using namespace waylaunch;
    std::vector<HyprClient> clients = {
        make_client("0xabc1", "helium", "New Tab - Helium"),
    };
    auto got = pick_hypr_address(clients, "helium", "");
    assert(got.has_value());
    assert(*got == "0xabc1");
    std::cout << "[PASS] class-only singleton\n";
}

void test_class_only_ambiguous_is_noop() {
    using namespace waylaunch;
    // Two same-class windows, no usable title: must NOT guess — focusing the
    // wrong instance lands on the wrong workspace.
    std::vector<HyprClient> clients = {
        make_client("0xabc1", "helium", ""),
        make_client("0xabc2", "helium", ""),
    };
    assert(!pick_hypr_address(clients, "helium", "").has_value());
    assert(!pick_hypr_address({}, "helium", "(17) WhatsApp - Helium").has_value());
    assert(!pick_hypr_address(clients, "", "").has_value());
    std::cout << "[PASS] ambiguous/empty is nullopt\n";
}

void test_lua_escape_and_payloads() {
    using namespace waylaunch;
    assert(hypr_lua_escape("plain") == "plain");
    assert(hypr_lua_escape("a\"b\\c") == "a\\\"b\\\\c");
    // Parens/dots pass through untouched: they are only special to regex, and
    // the address selector never runs a regex.
    assert(hypr_focus_payload_lua("0xabc1") == "hl.dsp.focus({window=\"address:0xabc1\"})");
    assert(hypr_focus_payload_stock("0xabc1") == "focuswindow address:0xabc1");
    std::cout << "[PASS] lua escape + payloads\n";
}

void test_address_validation() {
    using namespace waylaunch;
    assert(hypr_address_is_safe("0x5620c90633e0"));
    assert(hypr_address_is_safe("5620c90633e0"));
    assert(!hypr_address_is_safe(""));
    assert(!hypr_address_is_safe("title:(17) WhatsApp"));
    assert(!hypr_address_is_safe("0xabc1\"}) -- injected"));
    assert(!hypr_address_is_safe("0xabc1\\"));
    std::cout << "[PASS] address validation\n";
}

} // namespace

int main() {
    test_exact_match_with_regex_metachars_in_title();
    test_title_only_fallback_covers_class_skew();
    test_class_only_singleton();
    test_class_only_ambiguous_is_noop();
    test_lua_escape_and_payloads();
    test_address_validation();
    std::cout << "All hyprland_focus unit tests passed successfully!\n";
    return 0;
}
