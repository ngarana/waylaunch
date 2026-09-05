#include "waylaunch/dropdown/dropdown_state.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace waylaunch;

std::string temp_path(const std::string& tag) {
    auto stem = "waylaunch-dropdown-state-" + tag + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return (std::filesystem::temp_directory_path() / stem).string() + ".tsv";
}

void test_round_trip() {
    DropdownStateStore store(temp_path("roundtrip"));
    assert(store.load().empty()); // missing file is empty, not an error
    assert(store.save_slot("term", {.w = 1920, .h = 462}));
    assert(store.save_slot("notes", {.w = 800, .h = 600}));
    auto reloaded = store.load();
    assert(reloaded.size() == 2);
    assert(reloaded["term"].w == 1920 && reloaded["term"].h == 462);
    assert(reloaded["notes"].w == 800 && reloaded["notes"].h == 600);
    // Overwrite merges, unrelated slots survive.
    assert(store.save_slot("term", {.w = 100, .h = 100}));
    reloaded = store.load();
    assert(reloaded.size() == 2 && reloaded["term"].w == 100);
    assert(reloaded["notes"].h == 600);
    std::cout << "[PASS] round trip\n";
}

void test_rejects_bad_input() {
    assert(!DropdownStateStore(temp_path("x")).save_slot("", {.w = 1, .h = 1}));
    assert(!DropdownStateStore(temp_path("x")).save_slot("a\tb", {.w = 1, .h = 1}));
    assert(!DropdownStateStore(temp_path("x")).save_slot("term", {.w = 0, .h = 10}));
    assert(!DropdownStateStore(temp_path("x")).save_slot("term", {.w = 10, .h = -3}));

    std::string path = temp_path("malformed");
    {
        std::ofstream file(path);
        file << "term\t1920\t462\n"
             << "garbage line\n"
             << "notes\tnan\t600\n"
             << "empty\t\t\n"
             << "neg\t-5\t10\n";
    }
    auto states = DropdownStateStore(path).load();
    assert(states.size() == 1 && states["term"].h == 462);
    std::cout << "[PASS] rejects bad input\n";
}

void test_default_path_shape() {
    std::string path = DropdownStateStore::default_path();
    assert(path.size() > strlen("waylaunch/dropdown.tsv"));
    assert(path.compare(path.size() - strlen("waylaunch/dropdown.tsv"),
                        strlen("waylaunch/dropdown.tsv"), "waylaunch/dropdown.tsv") == 0);
    std::cout << "[PASS] default path shape\n";
}

int main() {
    test_round_trip();
    test_rejects_bad_input();
    test_default_path_shape();
    std::cout << "dropdown_state_test: all passed\n";
    return 0;
}
