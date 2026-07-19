#include "waylaunch/app_launcher.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace waylaunch {

AppLauncher::AppLauncher() = default;
AppLauncher::~AppLauncher() = default;

void AppLauncher::set_search_paths(const std::vector<std::string>& paths) {
    search_paths_ = paths;
}

std::vector<std::string> AppLauncher::get_desktop_dirs() const {
    std::vector<std::string> dirs;

    if (!search_paths_.empty()) {
        return search_paths_;
    }

    // Default XDG directories
    const char* data_home = getenv("XDG_DATA_HOME");
    if (data_home && data_home[0] != '\0') {
        dirs.push_back(std::string(data_home) + "/applications");
    }

    const char* home = getenv("HOME");
    if (home) {
        dirs.push_back(std::string(home) + "/.local/share/applications");
    }

    // System directories
    dirs.push_back("/usr/share/applications");
    dirs.push_back("/usr/local/share/applications");
    dirs.push_back("/usr/share/gnome/applications");
    dirs.push_back("/usr/share/gnome/apps");
    dirs.push_back("/usr/share/mate/applications");

    return dirs;
}

void AppLauncher::scan() {
    entries_.clear();

    for (const auto& dir : get_desktop_dirs()) {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".desktop") {
                parse_desktop_file(entry.path().string());
            }
        }
    }

    // Sort by name
    std::sort(entries_.begin(), entries_.end(),
        [](const DesktopEntry& a, const DesktopEntry& b) {
            return a.name < b.name;
        });
}

void AppLauncher::parse_desktop_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    DesktopEntry entry;
    entry.desktop_path = path;
    std::string line;
    bool in_desktop_entry = false;

    while (std::getline(file, line)) {
        // Remove carriage return
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Check for section header
        if (line[0] == '[') {
            in_desktop_entry = (line == "[Desktop Entry]");
            continue;
        }

        if (!in_desktop_entry) continue;

        // Parse key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (key == "Name") {
            entry.name = value;
        } else if (key == "Exec") {
            entry.exec = value;
        } else if (key == "Icon") {
            entry.icon = value;
        } else if (key == "Categories") {
            entry.categories = value;
        } else if (key == "Comment") {
            entry.comment = value;
        } else if (key == "GenericName") {
            entry.generic_name = value;
        } else if (key == "NoDisplay") {
            entry.no_display = (value == "true");
        } else if (key == "Hidden") {
            entry.hidden = (value == "true");
        }
    }

    // Skip entries without name or marked as hidden
    if (entry.name.empty() || entry.no_display || entry.hidden) {
        return;
    }

    // Clean up Exec field (remove %f, %u, etc.)
    size_t pos = entry.exec.find(" %");
    if (pos != std::string::npos) {
        entry.exec = entry.exec.substr(0, pos);
    }

    entries_.push_back(std::move(entry));
}

std::vector<DesktopEntry> AppLauncher::search(const std::string& query) const {
    if (query.empty()) {
        return entries_;
    }

    std::vector<DesktopEntry> results;
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    for (const auto& entry : entries_) {
        std::string name_lower = entry.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        std::string generic_lower = entry.generic_name;
        std::transform(generic_lower.begin(), generic_lower.end(), generic_lower.begin(), ::tolower);

        std::string comment_lower = entry.comment;
        std::transform(comment_lower.begin(), comment_lower.end(), comment_lower.begin(), ::tolower);

        std::string categories_lower = entry.categories;
        std::transform(categories_lower.begin(), categories_lower.end(), categories_lower.begin(), ::tolower);

        if (name_lower.find(query_lower) != std::string::npos ||
            generic_lower.find(query_lower) != std::string::npos ||
            comment_lower.find(query_lower) != std::string::npos ||
            categories_lower.find(query_lower) != std::string::npos) {
            results.push_back(entry);
        }
    }

    return results;
}

} // namespace waylaunch
