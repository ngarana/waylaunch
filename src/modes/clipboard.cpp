#include "waylaunch/clipboard.h"
#include "waylaunch/subprocess.h"
#include <string>
#include <vector>

namespace waylaunch {

// wl-copy forks a background process to keep serving the clipboard selection
// after it returns, and that daemon inherits (and never closes) our pipe fds.
// Subprocess::run() waits for those pipes to hit EOF, so with wl-copy it never
// returns — it blocks forever, and since every activation runs through here
// first, the whole launcher (and its exclusive keyboard grab) hangs with it.
// wl-copy takes the content as a trailing argument, so no stdin pipe is
// needed at all: spawn_detached() (fire-and-forget, no pipes) is correct here.
bool Clipboard::copy_text(const std::string& text) {
    if (!Subprocess::command_exists("wl-copy")) return false;
    Subprocess::spawn_detached({"wl-copy", "--type", "text/plain", text});
    return true;
}

bool Clipboard::copy_file_path(const std::string& path) {
    if (!Subprocess::command_exists("wl-copy")) return false;
    Subprocess::spawn_detached({"wl-copy", "--type", "text/uri-list", path});
    return true;
}

std::string Clipboard::paste_text() {
    if (!Subprocess::command_exists("wl-paste")) return "";
    std::vector<std::string> argv = {"wl-paste", "--no-newline"};
    auto result = Subprocess::run(argv);
    return result.stdout;
}

} // namespace waylaunch
