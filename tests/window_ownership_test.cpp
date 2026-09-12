#include "waylaunch/dropdown/window_ownership.h"

#include <cassert>
#include <iostream>
#include <map>

using namespace waylaunch;

namespace {

constexpr const char* kApp = "waylaunch-drop-term";

HyprClient client(const std::string& address, const std::string& klass, int pid) {
    HyprClient c;
    c.address = address;
    c.klass = klass;
    c.pid = pid;
    return c;
}

// Injected kinship: child -> parent, so tests never touch /proc.
AncestryFn kinship(std::map<int, int> parents) {
    return [parents = std::move(parents)](int pid, int ancestor) {
        for (int hops = 0; hops < 16 && pid > 0; ++hops) {
            if (pid == ancestor) return true;
            auto it = parents.find(pid);
            if (it == parents.end()) return false;
            pid = it->second;
        }
        return false;
    };
}

} // namespace

void test_tiers() {
    AncestryFn kin = kinship({{200, 100}});
    assert(classify_ownership(100, 100, kin) == Ownership::Exact);
    assert(classify_ownership(200, 100, kin) == Ownership::Descendant);
    assert(classify_ownership(999, 100, kin) == Ownership::ClassOnly);
    // An unknown owner cannot distinguish anything.
    assert(classify_ownership(100, -1, kin) == Ownership::ClassOnly);
    assert(classify_ownership(-1, 100, kin) == Ownership::ClassOnly);
    std::cout << "[PASS] tiers\n";
}

// The limitation this closes: an intruder listed FIRST must not be picked.
void test_intruder_never_selected() {
    std::vector<HyprClient> clients = {
        client("0xAA", kApp, 999), // hand-spawned, same class, listed first
        client("0xBB", kApp, 100), // the supervised terminal
    };
    size_t pick = select_owned(clients, kApp, 100, kinship({}));
    assert(pick == 1 && clients[pick].address == "0xBB");

    // ...and it is not a tab either.
    auto owned = filter_owned(clients, kApp, 100, kinship({}));
    assert(owned.size() == 1 && owned[0] == 1);
    std::cout << "[PASS] intruder never selected\n";
}

void test_descendant_counts_as_ours() {
    // A terminal that re-execs shows its window under a child pid.
    std::vector<HyprClient> clients = {
        client("0xAA", kApp, 999),
        client("0xBB", kApp, 200), // child of 100
    };
    AncestryFn kin = kinship({{200, 100}});
    assert(select_owned(clients, kApp, 100, kin) == 1);
    auto owned = filter_owned(clients, kApp, 100, kin);
    assert(owned.size() == 1 && owned[0] == 1);
    std::cout << "[PASS] descendant counts as ours\n";
}

void test_exact_beats_descendant() {
    std::vector<HyprClient> clients = {
        client("0xAA", kApp, 200), // descendant, listed first
        client("0xBB", kApp, 100), // the process itself
    };
    AncestryFn kin = kinship({{200, 100}});
    assert(select_owned(clients, kApp, 100, kin) == 1);
    // Both are tabs, in compositor order.
    auto owned = filter_owned(clients, kApp, 100, kin);
    assert(owned.size() == 2 && owned[0] == 0 && owned[1] == 1);
    std::cout << "[PASS] exact beats descendant\n";
}

// A restarted daemon has no pid yet and must still adopt the window it left.
void test_unknown_owner_adopts_first_match() {
    std::vector<HyprClient> clients = {
        client("0xAA", "firefox", 5),
        client("0xBB", kApp, 999),
        client("0xCC", kApp, 998),
    };
    assert(select_owned(clients, kApp, -1, kinship({})) == 1);
    auto owned = filter_owned(clients, kApp, -1, kinship({}));
    assert(owned.size() == 2); // no owner ⇒ every class match is a candidate
    std::cout << "[PASS] unknown owner adopts first match\n";
}

void test_no_match() {
    std::vector<HyprClient> clients = {client("0xAA", "firefox", 5)};
    assert(select_owned(clients, kApp, 100, kinship({})) == std::string::npos);
    assert(filter_owned(clients, kApp, 100, kinship({})).empty());
    assert(select_owned({}, kApp, 100, kinship({})) == std::string::npos);
    std::cout << "[PASS] no match\n";
}

// Every owned window is an intruder: nothing is selected rather than
// something wrong being placed.
void test_all_intruders_rejected_for_tabs() {
    std::vector<HyprClient> clients = {client("0xAA", kApp, 999), client("0xBB", kApp, 998)};
    assert(filter_owned(clients, kApp, 100, kinship({})).empty());
    // select_owned still falls back, so a mis-detected pid degrades to the
    // old behaviour instead of a dropdown that will not open at all.
    assert(select_owned(clients, kApp, 100, kinship({})) == 0);
    std::cout << "[PASS] all intruders rejected for tabs\n";
}

int main() {
    test_tiers();
    test_intruder_never_selected();
    test_descendant_counts_as_ours();
    test_exact_beats_descendant();
    test_unknown_owner_adopts_first_match();
    test_no_match();
    test_all_intruders_rejected_for_tabs();
    std::cout << "window_ownership_test: all passed\n";
    return 0;
}
