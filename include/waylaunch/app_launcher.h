#pragma once

#include <string>
#include <vector>

namespace waylaunch {

struct DesktopEntry {
    std::string name;
    std::string exec;
    std::string icon;
    std::string categories;
    std::string comment;
    std::string generic_name;
    std::string desktop_path;   // source .desktop file (for "reveal in files")
    bool no_display = false;
    bool hidden = false;
};

class AppLauncher {
public:
    AppLauncher();
    ~AppLauncher();

    void scan();
    void set_search_paths(const std::vector<std::string>& paths);

    std::vector<DesktopEntry> search(const std::string& query) const;

private:
    void parse_desktop_file(const std::string& path);
    std::vector<std::string> get_desktop_dirs() const;

    std::vector<DesktopEntry> entries_;
    std::vector<std::string> search_paths_;
};

} // namespace waylaunch
