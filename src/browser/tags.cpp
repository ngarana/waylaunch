#include "waylaunch/tags.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace waylaunch {

TagsManager::TagsManager() = default;
TagsManager::~TagsManager() = default;

bool TagsManager::load() {
    std::string path = get_tags_file_path();
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        auto j = nlohmann::json::parse(content);
        tags_.clear();

        if (j.contains("tags") && j["tags"].is_array()) {
            for (const auto& tag_json : j["tags"]) {
                Tag tag;
                tag.name = tag_json.value("name", "");
                tag.color = tag_json.value("color", "#808080");

                if (tag_json.contains("paths") && tag_json["paths"].is_array()) {
                    for (const auto& p : tag_json["paths"]) {
                        tag.paths.push_back(p.get<std::string>());
                    }
                }

                tags_.push_back(std::move(tag));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool TagsManager::save() {
    std::string path = get_tags_file_path();

    nlohmann::json j;
    nlohmann::json tags_array = nlohmann::json::array();

    for (const auto& tag : tags_) {
        nlohmann::json tag_json;
        tag_json["name"] = tag.name;
        tag_json["color"] = tag.color;
        tag_json["paths"] = tag.paths;
        tags_array.push_back(tag_json);
    }

    j["tags"] = tags_array;

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << j.dump(2);
    return file.good();
}

std::vector<Tag> TagsManager::get_tags() const {
    return tags_;
}

Tag TagsManager::get_tag(const std::string& name) const {
    for (const auto& tag : tags_) {
        if (tag.name == name) return tag;
    }
    return Tag{};
}

bool TagsManager::has_tag(const std::string& name) const {
    return std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == name; }) != tags_.end();
}

bool TagsManager::create_tag(const std::string& name, const std::string& color) {
    if (has_tag(name)) return false;

    Tag tag;
    tag.name = name;
    tag.color = color;
    tags_.push_back(std::move(tag));
    return save();
}

bool TagsManager::delete_tag(const std::string& name) {
    auto it = std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == name; });

    if (it == tags_.end()) return false;

    tags_.erase(it);
    return save();
}

bool TagsManager::rename_tag(const std::string& old_name, const std::string& new_name) {
    if (has_tag(new_name)) return false;

    auto it = std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == old_name; });

    if (it == tags_.end()) return false;

    it->name = new_name;
    return save();
}

bool TagsManager::set_tag_color(const std::string& name, const std::string& color) {
    auto it = std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == name; });

    if (it == tags_.end()) return false;

    it->color = color;
    return save();
}

bool TagsManager::tag_file(const std::string& file_path, const std::string& tag_name) {
    auto it = std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == tag_name; });

    if (it == tags_.end()) return false;

    if (std::find(it->paths.begin(), it->paths.end(), file_path) != it->paths.end()) {
        return true;
    }

    it->paths.push_back(file_path);
    return save();
}

bool TagsManager::untag_file(const std::string& file_path, const std::string& tag_name) {
    auto it = std::find_if(tags_.begin(), tags_.end(),
        [&](const Tag& t) { return t.name == tag_name; });

    if (it == tags_.end()) return false;

    auto path_it = std::find(it->paths.begin(), it->paths.end(), file_path);
    if (path_it == it->paths.end()) return false;

    it->paths.erase(path_it);
    return save();
}

bool TagsManager::is_file_tagged(const std::string& file_path, const std::string& tag_name) const {
    for (const auto& tag : tags_) {
        if (tag.name == tag_name) {
            return std::find(tag.paths.begin(), tag.paths.end(), file_path) != tag.paths.end();
        }
    }
    return false;
}

std::vector<std::string> TagsManager::get_file_tags(const std::string& file_path) const {
    std::vector<std::string> result;
    for (const auto& tag : tags_) {
        if (std::find(tag.paths.begin(), tag.paths.end(), file_path) != tag.paths.end()) {
            result.push_back(tag.name);
        }
    }
    return result;
}

std::vector<std::string> TagsManager::get_files_by_tag(const std::string& tag_name) const {
    for (const auto& tag : tags_) {
        if (tag.name == tag_name) return tag.paths;
    }
    return {};
}

std::vector<Tag> TagsManager::search_tags(const std::string& query) const {
    std::vector<Tag> result;
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    for (const auto& tag : tags_) {
        std::string name_lower = tag.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        if (name_lower.find(query_lower) != std::string::npos) {
            result.push_back(tag);
        }
    }
    return result;
}

std::vector<std::string> TagsManager::get_default_tag_colors() {
    return {
        "#ff3b30",
        "#ff9500",
        "#ffcc00",
        "#34c759",
        "#007aff",
        "#5856d6",
        "#af52de",
        "#8e8e93"
    };
}

std::string TagsManager::get_tags_file_path() const {
    const char* home = std::getenv("HOME");
    if (!home) return "/tmp/tags.json";
    return std::string(home) + "/.local/share/waylaunch/tags.json";
}

TagMenu::TagMenu() = default;
TagMenu::~TagMenu() = default;

void TagMenu::set_visible(bool visible) {
    visible_ = visible;
}

bool TagMenu::is_visible() const {
    return visible_;
}

void TagMenu::set_position(int x, int y) {
    x_ = x;
    y_ = y;
}

void TagMenu::set_file_path(const std::string& path) {
    file_path_ = path;
}

void TagMenu::set_file_tags(const std::vector<std::string>& tags) {
    file_tags_ = tags;
}

int TagMenu::hit_test(double x, double y) const {
    if (!visible_) return -1;

    if (x < x_ || x >= x_ + MENU_WIDTH) return -1;
    if (y < y_ || y >= y_ + (all_tags_.size() + 1) * ITEM_HEIGHT + PADDING * 2) return -1;

    int current_y = y_ + PADDING;

    for (size_t i = 0; i < all_tags_.size(); i++) {
        if (y >= current_y && y < current_y + ITEM_HEIGHT) {
            return static_cast<int>(i);
        }
        current_y += ITEM_HEIGHT;
    }

    if (y >= current_y && y < current_y + ITEM_HEIGHT) {
        return static_cast<int>(all_tags_.size());
    }

    return -1;
}

void TagMenu::render(Renderer& renderer, int, int, int, int, const Theme& theme) {
    if (!visible_) return;

    TagsManager tags_manager;
    tags_manager.load();
    all_tags_ = tags_manager.get_tags();

    int height = all_tags_.size() * ITEM_HEIGHT + ITEM_HEIGHT + PADDING * 2;
    renderer.rounded_rect(x_, y_, MENU_WIDTH, height, theme.corner_radius, theme.background_alt);

    int current_y = y_ + PADDING;

    for (size_t i = 0; i < all_tags_.size(); i++) {
        const auto& tag = all_tags_[i];

        bool is_tagged = std::find(file_tags_.begin(), file_tags_.end(), tag.name) != file_tags_.end();

        if (is_tagged) {
            renderer.fill_rect(x_ + 2, current_y, MENU_WIDTH - 4, ITEM_HEIGHT, theme.selection);
        }

        int dot_x = x_ + PADDING;
        int dot_y = current_y + (ITEM_HEIGHT - DOT_SIZE) / 2;
        Color tag_color = Color::from_hex(tag.color);
        renderer.rounded_rect(dot_x, dot_y, DOT_SIZE, DOT_SIZE, DOT_SIZE / 2, tag_color);

        std::vector<TextSegment> segments;
        if (is_tagged) {
            segments.push_back({"✓ ", theme.accent});
        }
        segments.push_back({tag.name, theme.foreground});
        renderer.draw_text_segments(dot_x + DOT_SIZE + 8, current_y + 6, segments, theme.result_font);

        current_y += ITEM_HEIGHT;
    }

    renderer.fill_rect(x_ + PADDING, current_y, MENU_WIDTH - 2 * PADDING, 1, theme.border);
    current_y += PADDING;

    renderer.draw_text(x_ + PADDING, current_y + 6, "+ New Tag", theme.result_font, theme.accent);
}

void TagMenu::set_tag_toggle_callback(TagToggleCallback callback) {
    tag_toggle_callback_ = callback;
}

void TagMenu::set_create_tag_callback(CreateTagCallback callback) {
    create_tag_callback_ = callback;
}

} // namespace waylaunch
