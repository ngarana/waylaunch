#include "waylaunch/dropdown/hyprland_events.h"

#include <cassert>
#include <iostream>

using namespace waylaunch;

// Recorded shape from the live `.socket2.sock` (Hyprland 0.56.2).
const char* kFixture =
    "activewindowv2>>55ce852058b0\n"
    "windowtitlev2>>55ce852058b0,\xe2\x97\x91 Dropdown terminal replacement\n"
    "activewindow>>kitty,\xe2\x97\x91 Dropdown terminal replacement\n"
    "closewindow>>55ce852058b0\n"
    "focusedmon>>eDP-1,0\n";

void test_whole_fixture() {
    EventLineParser parser;
    auto events = parser.push(kFixture, std::char_traits<char>::length(kFixture));
    assert(events.size() == 5);
    assert(events[0].name == "activewindowv2" && events[0].payload == "55ce852058b0");
    assert(events[1].name == "windowtitlev2");
    assert(events[2].name == "activewindow" &&
           events[2].payload == "kitty,\xe2\x97\x91 Dropdown terminal replacement");
    assert(events[3].name == "closewindow" && events[3].payload == "55ce852058b0");
    assert(events[4].name == "focusedmon" && events[4].payload == "eDP-1,0");
    std::cout << "[PASS] whole fixture\n";
}

void test_split_writes_reassemble() {
    // One byte at a time: every chunk boundary must still frame correctly.
    EventLineParser parser;
    std::vector<HyprEvent> events;
    for (const char* p = kFixture; *p != '\0'; ++p) {
        auto chunk = parser.push(p, 1);
        events.insert(events.end(), chunk.begin(), chunk.end());
    }
    assert(events.size() == 5);
    assert(events[0].name == "activewindowv2");
    assert(events[4].name == "focusedmon");
    std::cout << "[PASS] split writes reassemble\n";
}

void test_partial_line_held() {
    EventLineParser parser;
    const char* part1 = "activewindowv2>>55ce85";
    auto events = parser.push(part1, std::char_traits<char>::length(part1));
    assert(events.empty()); // no newline yet: nothing emitted
    const char* part2 = "2058b0\nclosewindow>>";
    events = parser.push(part2, std::char_traits<char>::length(part2));
    assert(events.size() == 1 && events[0].payload == "55ce852058b0");
    std::cout << "[PASS] partial line held\n";
}

void test_separatorless_line_dropped() {
    EventLineParser parser;
    const char* garbage = "notaevent\nactivewindowv2>>abc\n";
    auto events = parser.push(garbage, std::char_traits<char>::length(garbage));
    assert(events.size() == 1 && events[0].name == "activewindowv2");
    std::cout << "[PASS] separatorless line dropped\n";
}

int main() {
    test_whole_fixture();
    test_split_writes_reassemble();
    test_partial_line_held();
    test_separatorless_line_dropped();
    std::cout << "hyprland_events_test: all passed\n";
    return 0;
}
