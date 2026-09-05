#include "waylaunch/dropdown/hyprland_json.h"

#include <cassert>
#include <iostream>

using namespace waylaunch;

// Shapes mirror live `hyprctl -j` output (Hyprland 0.56.2), including a
// hostile title carrying braces, quotes, and escapes to prove the skipper.
const char* kClients = R"([
{"address":"0x55ce851fc6c0","mapped":true,"hidden":false,"at":[0,0],
 "size":[1920,480],"workspace":{"id":-98,"name":"special:dropterm"},
 "floating":true,"monitor":0,"class":"dropterm",
 "title":"arch@host:~/x } \"quoted\" \u25d1 end","initialClass":"dropterm",
 "pid":3130,"xwayland":false,"pinned":false,"fullscreen":0,
 "grouped":[],"tags":[],"focusHistoryID":4},
{"address":"0x55ce852058b0","mapped":true,"hidden":false,"at":[654,514],
 "size":[1200,700],"workspace":{"id":4,"name":"4"},
 "floating":true,"monitor":0,"class":"kitty","title":"plain",
 "initialClass":"kitty","pid":818290,"xwayland":false,"pinned":false,
 "fullscreen":0,"grouped":[],"tags":[],"focusHistoryID":2}
])";

const char* kMonitors = R"([
 {"id":0,"name":"eDP-1","description":"Some Panel","width":1920,"height":1200,
 "x":0,"y":0,"activeWorkspace":{"id":2,"name":"2"},
 "specialWorkspace":{"id":0,"name":""},"reserved":[0,44,0,0],"scale":1.0,
 "transform":0,"focused":true,"dpmsStatus":true,"vrr":false,"disabled":false,
 "availableModes":["1920x1200@60.00Hz"]}
])";

void test_clients_shape() {
    auto clients = parse_hypr_clients(kClients);
    assert(clients.size() == 2);
    assert(clients[0].address == "0x55ce851fc6c0");
    assert(clients[0].klass == "dropterm");
    assert(clients[0].pid == 3130);
    assert(clients[0].at_x == 0 && clients[0].at_y == 0);
    assert(clients[0].width == 1920 && clients[0].height == 480);
    assert(clients[0].workspace_id == -98); // special workspaces go negative
    assert(clients[0].workspace_name == "special:dropterm");
    assert(clients[0].floating && clients[0].mapped);
    assert(clients[1].klass == "kitty");
    assert(clients[1].at_x == 654 && clients[1].at_y == 514);
    assert(clients[1].workspace_id == 4);
    std::cout << "[PASS] clients shape\n";
}

void test_monitors_shape() {
    auto monitors = parse_hypr_monitors(kMonitors);
    assert(monitors.size() == 1);
    assert(monitors[0].name == "eDP-1");
    assert(monitors[0].width == 1920 && monitors[0].height == 1200);
    assert(monitors[0].active_workspace == 2);
    assert(monitors[0].reserved_top == 44);
    assert(monitors[0].reserved_bottom == 0);
    assert(monitors[0].focused);
    // Order is [left, top, right, bottom]: a bottom inset lands at index 3.
    auto bottom_bar = parse_hypr_monitors(R"([{"name":"HDMI-1","reserved":[0,0,0,20]}])");
    assert(bottom_bar.size() == 1);
    assert(bottom_bar[0].reserved_top == 0);
    assert(bottom_bar[0].reserved_bottom == 20);
    std::cout << "[PASS] monitors shape\n";
}

void test_empty_and_malformed() {
    assert(parse_hypr_clients("[]").empty());
    assert(parse_hypr_monitors("[]").empty());
    assert(parse_hypr_clients("").empty());
    // Truncated payload yields what was completed, never a crash.
    auto partial = parse_hypr_clients(R"([{"address":"0x1","class":)");
    assert(partial.empty());
    std::cout << "[PASS] empty and malformed\n";
}

void test_missing_fields_default() {
    auto clients = parse_hypr_clients(R"([{"address":"0xabc"}])");
    assert(clients.size() == 1);
    assert(clients[0].address == "0xabc");
    assert(clients[0].klass.empty() && clients[0].pid == -1);
    assert(!clients[0].floating && !clients[0].mapped);
    std::cout << "[PASS] missing fields default\n";
}

int main() {
    test_clients_shape();
    test_monitors_shape();
    test_empty_and_malformed();
    test_missing_fields_default();
    std::cout << "hyprland_json_test: all passed\n";
    return 0;
}
