#pragma once

#include <string>

namespace waylaunch {

// What a result represents — drives its action (launch/copy/open) and grouping.
// Content = a file matched by its indexed *contents* (the CONTENTS section).
enum class ItemKind { Application, File, Folder, Calculator, Command, Content, History };

struct ListItem {
    std::string name;            // primary label
    std::string path;            // app: exec line · file/folder: fs path · calc: result payload
    std::string description;     // subtitle (comment / abbreviated path / expression)
    std::string icon_name;
    std::string action_command;  // custom command, if any
    std::string reveal_path;     // filesystem path to reveal on right-click (app → .desktop)
    std::string snippet;         // content hit: highlighted excerpt of the matched body
    ItemKind    kind = ItemKind::Application;
    float       score = 0.0f;
};

} // namespace waylaunch
