#pragma once

#include <string>

namespace waylaunch {

class Clipboard {
public:
    static bool copy_text(const std::string& text);
    static bool copy_file_path(const std::string& path);
    static std::string paste_text();
};

} // namespace waylaunch
