#include "waylaunch/info_panel.h"
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <filesystem>
#include <dirent.h>

namespace waylaunch {

InfoPanel::InfoPanel() = default;
InfoPanel::~InfoPanel() = default;

void InfoPanel::set_width(int width) {
    width_ = width;
}

int InfoPanel::width() const {
    return width_;
}

void InfoPanel::set_visible(bool visible) {
    visible_ = visible;
}

bool InfoPanel::is_visible() const {
    return visible_;
}

void InfoPanel::load_file(const std::string& path) {
    load_file_info(path);
}

void InfoPanel::clear() {
    data_ = InfoPanelData();
}

const InfoPanelData& InfoPanel::data() const {
    return data_;
}

void InfoPanel::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    if (!visible_) return;

    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int padding = 16;
    int current_y = y + padding;
    int line_height = 20;

    std::vector<TextSegment> name_segments;
    name_segments.push_back({data_.name, theme.foreground});
    renderer.draw_text_segments(x + padding, current_y, name_segments, theme.result_font);
    current_y += line_height + 8;

    auto draw_field = [&](const std::string& key, const std::string& value) {
        renderer.draw_text(x + padding, current_y, key + ":", theme.result_detail_font, theme.text_muted);
        renderer.draw_text(x + padding + 100, current_y, value, theme.result_detail_font, theme.foreground);
        current_y += line_height;
    };

    draw_field("Kind", data_.kind);
    draw_field("Size", data_.size + " (" + data_.size_bytes + " bytes)");
    draw_field("Where", data_.path);
    draw_field("Created", data_.created);
    draw_field("Modified", data_.modified);
    draw_field("Accessed", data_.accessed);

    current_y += 8;

    draw_field("Permissions", data_.permissions);
    draw_field("Owner", data_.owner);
    draw_field("Group", data_.group);

    if (data_.is_directory) {
        current_y += 8;
        draw_field("Items", std::to_string(data_.item_count));
    }

    if (!data_.tags.empty()) {
        current_y += 8;
        renderer.draw_text(x + padding, current_y, "Tags:", theme.result_detail_font, theme.text_muted);
        current_y += line_height;

        for (const auto& tag : data_.tags) {
            renderer.draw_text(x + padding + 16, current_y, tag, theme.result_font, theme.foreground);
            current_y += line_height;
        }
    }
}

void InfoPanel::load_file_info(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        data_.name = "Error";
        data_.path = path;
        data_.kind = "Cannot access file";
        return;
    }

    data_.path = path;
    data_.name = std::filesystem::path(path).filename().string();
    data_.is_directory = S_ISDIR(st.st_mode);
    data_.is_symlink = S_ISLNK(st.st_mode);

    if (data_.is_directory) {
        data_.kind = "Folder";
        int count = 0;
        DIR* dir = opendir(path.c_str());
        if (dir) {
            while (readdir(dir)) count++;
            closedir(dir);
            count -= 2;
        }
        data_.item_count = std::max(0, count);
    } else {
        data_.kind = FileModel::file_kind_from_path(path);
    }

    data_.size = format_size(st.st_size);
    data_.size_bytes = std::to_string(st.st_size);
    data_.created = format_date(st.st_ctime);
    data_.modified = format_date(st.st_mtime);
    data_.accessed = format_date(st.st_atime);
    data_.permissions = format_permissions(st.st_mode);

    struct passwd* pw = getpwuid(st.st_uid);
    data_.owner = pw ? pw->pw_name : std::to_string(st.st_uid);

    struct group* gr = getgrgid(st.st_gid);
    data_.group = gr ? gr->gr_name : std::to_string(st.st_gid);

    TagsManager tags;
    tags.load();
    data_.tags = tags.get_file_tags(path);
}

std::string InfoPanel::format_size(off_t size) const {
    if (size < 1024) return std::to_string(size) + " B";
    if (size < 1024 * 1024) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (size / 1024.0) << " KB";
        return oss.str();
    }
    if (size < 1024 * 1024 * 1024) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (size / (1024.0 * 1024.0)) << " MB";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (size / (1024.0 * 1024.0 * 1024.0)) << " GB";
    return oss.str();
}

std::string InfoPanel::format_date(time_t time) const {
    char buf[64];
    struct tm* tm_info = localtime(&time);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

std::string InfoPanel::format_permissions(mode_t mode) const {
    std::string perms;
    perms += (mode & S_IRUSR) ? 'r' : '-';
    perms += (mode & S_IWUSR) ? 'w' : '-';
    perms += (mode & S_IXUSR) ? 'x' : '-';
    perms += (mode & S_IRGRP) ? 'r' : '-';
    perms += (mode & S_IWGRP) ? 'w' : '-';
    perms += (mode & S_IXGRP) ? 'x' : '-';
    perms += (mode & S_IROTH) ? 'r' : '-';
    perms += (mode & S_IWOTH) ? 'w' : '-';
    perms += (mode & S_IXOTH) ? 'x' : '-';

    perms += " (" + std::to_string(mode & 0777) + ")";
    return perms;
}

} // namespace waylaunch
