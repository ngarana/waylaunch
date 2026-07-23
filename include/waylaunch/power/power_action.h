#pragma once

#include <string>
#include <vector>

namespace waylaunch {

// One power action as shown in the overlay. Pure data: the backend builds these
// from defaults + config overrides; manager/renderer only read them.
struct PowerAction {
    std::string id;                  // "lock" | "restart" | "exit" | ...
    std::string name;                // display label ("Shut Down")
    std::vector<std::string> argv;   // argv-split command — never shell-joined
    std::string icon_name;           // freedesktop icon
    std::string confirm_text;        // full dialog headline (composed by backend)
    std::string subtext;             // dialog body; non-empty ⇒ session-ending
                                     //   (drives the red confirm button too)
    bool destructive = false;        // gate behind the confirmation dialog
};

} // namespace waylaunch
