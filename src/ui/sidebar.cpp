#include "waylaunch/sidebar.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

namespace waylaunch {

Sidebar::Sidebar() = default;
Sidebar::~Sidebar() = default;

void Sidebar::set_width(int width) {
    width_ = width;
}

int Sidebar::width() const {
    return width_;
}

void Sidebar::set_selected_path(const std::string& path) {
    selected_path_ = path;
}

const std::string& Sidebar::selected_path() const {
    return selected_path_;
}

void Sidebar::load_favorites() {
    items_.clear();

    items_.push_back({"Favorites", "", "folder-favorites", true, true, false, 0});
    items_.push_back({"Home", FileModel::home_directory(), "user-home", false, false, false, 1});
    items_.push_back({"Desktop", FileModel::desktop_directory(), "user-desktop", false, false, false, 1});
    items_.push_back({"Documents", FileModel::documents_directory(), "folder-documents", false, false, false, 1});
    items_.push_back({"Downloads", FileModel::downloads_directory(), "folder-download", false, false, false, 1});

    load_bookmarks();
}

void Sidebar::load_bookmarks() {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::string bookmark_path = std::string(home) + "/.config/gtk-3.0/bookmarks";
    std::ifstream file(bookmark_path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t space = line.find(' ');
        std::string path = (space != std::string::npos) ? line.substr(0, space) : line;
        std::string name = (space != std::string::npos) ? line.substr(space + 1) : "";

        if (name.empty()) {
            name = std::filesystem::path(path).filename().string();
        }

        if (std::find_if(items_.begin(), items_.end(), [&](const SidebarItem& i) { return i.path == path; }) == items_.end()) {
            items_.push_back({name, path, "folder", false, false, false, 1});
        }
    }
}

void Sidebar::load_locations() {
    items_.push_back({"Locations", "", "computer", true, true, false, 0});
    items_.push_back({"Computer", "/", "computer", false, false, false, 1});
    items_.push_back({"Network", "/mnt", "network-workgroup", false, false, false, 1});

    std::ifstream mounts("/proc/mounts");
    if (mounts.is_open()) {
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string device, mount_point, fs_type;
            iss >> device >> mount_point >> fs_type;

            if (mount_point.find("/media/") == 0 || mount_point.find("/mnt/") == 0) {
                if (std::find_if(items_.begin(), items_.end(), [&](const SidebarItem& i) { return i.path == mount_point; }) == items_.end()) {
                    std::string name = std::filesystem::path(mount_point).filename().string();
                    if (name.empty()) name = mount_point;
                    items_.push_back({name, mount_point, "drive-harddisk", false, false, false, 1});
                }
            }
        }
    }
}

void Sidebar::load_tags() {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::string tags_path = std::string(home) + "/.local/share/waylaunch/tags.json";
    std::ifstream file(tags_path);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        auto j = nlohmann::json::parse(content);
        if (j.contains("tags") && j["tags"].is_array()) {
            for (const auto& tag : j["tags"]) {
                TagItem ti;
                ti.name = tag.value("name", "");
                ti.color = tag.value("color", "#808080");
                if (tag.contains("paths") && tag["paths"].is_array()) {
                    for (const auto& p : tag["paths"]) {
                        ti.paths.push_back(p.get<std::string>());
                    }
                }
                tags_.push_back(std::move(ti));
            }
        }
    } catch (...) {}
}

void Sidebar::load_recent_files(int max_count) {
    std::vector<std::string> recent_paths;

    const char* home = std::getenv("HOME");
    if (home) {
        std::string recent_path = std::string(home) + "/.local/share/recently-used.xbel";
        std::ifstream file(recent_path);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            size_t pos = 0;
            while ((pos = content.find("href=\"file://", pos)) != std::string::npos) {
                pos += 13;
                size_t end = content.find("\"", pos);
                if (end != std::string::npos) {
                    std::string path = content.substr(pos, end - pos);
                    if (std::find(recent_paths.begin(), recent_paths.end(), path) == recent_paths.end()) {
                        recent_paths.push_back(path);
                        if (static_cast<int>(recent_paths.size()) >= max_count) break;
                    }
                }
            }
        }
    }

    if (!recent_paths.empty()) {
        items_.push_back({"Recents", "", "document-open-recent", true, true, false, 0});
        for (const auto& path : recent_paths) {
            std::string name = std::filesystem::path(path).filename().string();
            items_.push_back({name, path, "document", false, false, false, 1});
        }
    }
}

void Sidebar::add_favorite(const std::string& path, const std::string& name) {
    if (std::find_if(items_.begin(), items_.end(), [&](const SidebarItem& i) { return i.path == path; }) == items_.end()) {
        items_.insert(items_.begin() + 1, {name, path, "folder", false, false, false, 1});
    }
}

void Sidebar::remove_favorite(const std::string& path) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [&](const SidebarItem& i) { return i.path == path; }), items_.end());
}

void Sidebar::add_tag(const std::string& name, const std::string& color) {
    if (std::find_if(tags_.begin(), tags_.end(), [&](const TagItem& t) { return t.name == name; }) == tags_.end()) {
        tags_.push_back({name, color, {}});
    }
}

void Sidebar::remove_tag(const std::string& name) {
    tags_.erase(std::remove_if(tags_.begin(), tags_.end(),
        [&](const TagItem& t) { return t.name == name; }), tags_.end());
}

void Sidebar::add_file_tag(const std::string& file_path, const std::string& tag_name) {
    auto it = std::find_if(tags_.begin(), tags_.end(), [&](const TagItem& t) { return t.name == tag_name; });
    if (it != tags_.end()) {
        if (std::find(it->paths.begin(), it->paths.end(), file_path) == it->paths.end()) {
            it->paths.push_back(file_path);
        }
    }
}

void Sidebar::remove_file_tag(const std::string& file_path, const std::string& tag_name) {
    auto it = std::find_if(tags_.begin(), tags_.end(), [&](const TagItem& t) { return t.name == tag_name; });
    if (it != tags_.end()) {
        it->paths.erase(std::remove(it->paths.begin(), it->paths.end(), file_path), it->paths.end());
    }
}

std::vector<TagItem> Sidebar::get_tags() const {
    return tags_;
}

int Sidebar::hit_test(double x, double y) const {
    if (x < 0 || x >= width_) return -1;

    int current_y = PADDING;
    for (size_t i = 0; i < items_.size(); i++) {
        const auto& item = items_[i];
        int h = item.is_section ? SECTION_HEIGHT : ITEM_HEIGHT;

        if (y >= current_y && y < current_y + h) {
            return static_cast<int>(i);
        }
        current_y += h;
    }

    if (!tags_.empty()) {
        current_y += 8;
        if (y >= current_y && y < current_y + SECTION_HEIGHT) {
            return -2;
        }
        current_y += SECTION_HEIGHT;

        for (size_t i = 0; i < tags_.size(); i++) {
            if (y >= current_y && y < current_y + TAG_HEIGHT) {
                return -static_cast<int>(i) - 10;
            }
            current_y += TAG_HEIGHT;
        }
    }

    return -1;
}

const SidebarItem* Sidebar::get_item(int index) const {
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        return &items_[index];
    }
    return nullptr;
}

void Sidebar::toggle_section(int index) {
    if (index >= 0 && index < static_cast<int>(items_.size()) && items_[index].is_section) {
        items_[index].is_expanded = !items_[index].is_expanded;
    }
}

void Sidebar::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int current_y = y + PADDING;

    for (size_t i = 0; i < items_.size(); i++) {
        const auto& item = items_[i];

        if (item.is_section) {
            if (!item.is_expanded) {
                current_y += SECTION_HEIGHT;
                continue;
            }
            render_section(renderer, x, current_y, w, item, theme);
            current_y += SECTION_HEIGHT;
        } else {
            if (current_y + ITEM_HEIGHT > y + h) break;

            bool is_selected = (item.path == selected_path_);
            render_item(renderer, x, current_y, w, item, is_selected, theme);
            current_y += ITEM_HEIGHT;
        }
    }

    if (!tags_.empty()) {
        render_separator(renderer, x, current_y, w, theme);
        current_y += 8;

        SidebarItem tag_section = {"Tags", "", "tag", true, true, false, 0};
        render_section(renderer, x, current_y, w, tag_section, theme);
        current_y += SECTION_HEIGHT;

        for (const auto& tag : tags_) {
            if (current_y + TAG_HEIGHT > y + h) break;
            render_tag_item(renderer, x, current_y, w, tag, theme);
            current_y += TAG_HEIGHT;
        }
    }
}

void Sidebar::render_section(Renderer& renderer, int x, int y, int, const SidebarItem& item, const Theme& theme) {
    std::string arrow = item.is_expanded ? "▾ " : "▸ ";
    std::vector<TextSegment> segments;
    segments.push_back({arrow + item.name, theme.text_muted});
    renderer.draw_text_segments(x + PADDING, y + 8, segments, theme.result_detail_font);
}

void Sidebar::render_item(Renderer& renderer, int x, int y, int w, const SidebarItem& item, bool selected, const Theme& theme) {
    if (selected) {
        renderer.fill_rect(x, y, w, ITEM_HEIGHT, theme.selection);
    }

    int text_x = x + PADDING + item.indent_level * 16;
    std::vector<TextSegment> segments;
    segments.push_back({item.name, selected ? theme.foreground : theme.text_muted});
    renderer.draw_text_segments(text_x, y + 6, segments, theme.result_font);
}

void Sidebar::render_tag_item(Renderer& renderer, int x, int y, int, const TagItem& tag, const Theme& theme) {
    int dot_size = 10;
    int dot_x = x + PADDING + 16;
    int dot_y = y + (TAG_HEIGHT - dot_size) / 2;

    Color tag_color = Color::from_hex(tag.color);
    renderer.rounded_rect(dot_x, dot_y, dot_size, dot_size, dot_size / 2, tag_color);

    std::vector<TextSegment> segments;
    segments.push_back({tag.name, theme.text_muted});
    renderer.draw_text_segments(dot_x + dot_size + 8, y + 4, segments, theme.result_font);
}

void Sidebar::render_separator(Renderer& renderer, int x, int y, int w, const Theme& theme) {
    renderer.fill_rect(x + PADDING, y, w - 2 * PADDING, 1, theme.border);
}

} // namespace waylaunch
