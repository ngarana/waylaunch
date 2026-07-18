#include "waylaunch/clipboard.h"
#include "waylaunch/subprocess.h"
#include <vector>
#include <string>

namespace waylaunch {

bool Clipboard::copy_text(const std::string& text) {
    if (!Subprocess::command_exists("wl-copy")) return false;
    std::vector<std::string> argv = {"wl-copy", "--type", "text/plain"};
    auto result = Subprocess::run(argv, text);
    return result.exit_code == 0;
}

bool Clipboard::copy_file_path(const std::string& path) {
    if (!Subprocess::command_exists("wl-copy")) return false;
    std::vector<std::string> argv = {"wl-copy", "--type", "text/uri-list"};
    auto result = Subprocess::run(argv, path);
    return result.exit_code == 0;
}

std::string Clipboard::paste_text() {
    if (!Subprocess::command_exists("wl-paste")) return "";
    std::vector<std::string> argv = {"wl-paste", "--no-newline"};
    auto result = Subprocess::run(argv);
    return result.stdout;
}

} // namespace waylaunch
