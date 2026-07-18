#include "waylaunch/preview.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

namespace waylaunch {

Preview::Preview() = default;
Preview::~Preview() = default;

void Preview::set_width(int width) {
    width_ = width;
}

int Preview::width() const {
    return width_;
}

void Preview::set_visible(bool visible) {
    visible_ = visible;
}

bool Preview::is_visible() const {
    return visible_;
}

void Preview::load_file(const std::string& path) {
    clear();

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        content_.error = "Cannot access file";
        return;
    }

    file_entry_.path = path;
    file_entry_.name = std::filesystem::path(path).filename().string();
    file_entry_.size = st.st_size;
    file_entry_.modified_time = st.st_mtime;
    file_entry_.is_directory = S_ISDIR(st.st_mode);
    file_entry_.is_symlink = S_ISLNK(st.st_mode);
    file_entry_.extension = std::filesystem::path(path).extension().string();

    content_.file_path = path;
    content_.type = detect_type(path);
    content_.mime_type = detect_mime_type(path);

    switch (content_.type) {
        case PreviewType::Text:
        case PreviewType::Code:
            load_text_content(path);
            load_code_content(path);
            break;
        case PreviewType::Image:
            load_image_content(path);
            break;
        default:
            break;
    }

    load_metadata(path);
}

void Preview::clear() {
    content_ = PreviewContent();
    file_entry_ = FileEntry();
}

const PreviewContent& Preview::content() const {
    return content_;
}

const FileEntry& Preview::file_entry() const {
    return file_entry_;
}

void Preview::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    if (!visible_) return;

    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int padding = 16;
    int current_y = y + padding;

    std::vector<TextSegment> name_segments;
    name_segments.push_back({file_entry_.name, theme.foreground});
    renderer.draw_text_segments(x + padding, current_y, name_segments, theme.result_font);
    current_y += 24;

    if (!content_.error.empty()) {
        renderer.draw_text(x + padding, current_y, content_.error, theme.result_font, theme.error);
        return;
    }

    int preview_h = h / 2;
    if (content_.type == PreviewType::Text || content_.type == PreviewType::Code) {
        render_text_preview(renderer, x + padding, current_y, w - 2 * padding, preview_h, theme);
        current_y += preview_h + 16;
    }

    render_metadata(renderer, x + padding, current_y, w - 2 * padding, theme);
}

PreviewType Preview::detect_type(const std::string& path) const {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::vector<std::string> text_exts = {
        ".txt", ".md", ".rst", ".log", ".csv", ".json", ".xml", ".yaml", ".yml", ".toml"
    };

    static const std::vector<std::string> code_exts = {
        ".c", ".cpp", ".h", ".hpp", ".py", ".js", ".ts", ".java", ".rs", ".go",
        ".sh", ".bash", ".zsh", ".css", ".html", ".sql", ".r", ".lua", ".rb"
    };

    static const std::vector<std::string> image_exts = {
        ".jpg", ".jpeg", ".png", ".gif", ".svg", ".webp", ".bmp", ".ico"
    };

    if (std::find(text_exts.begin(), text_exts.end(), ext) != text_exts.end()) return PreviewType::Text;
    if (std::find(code_exts.begin(), code_exts.end(), ext) != code_exts.end()) return PreviewType::Code;
    if (std::find(image_exts.begin(), image_exts.end(), ext) != image_exts.end()) return PreviewType::Image;
    if (ext == ".pdf") return PreviewType::PDF;
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") return PreviewType::Video;
    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg") return PreviewType::Audio;
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".bz2" || ext == ".xz") return PreviewType::Archive;

    return PreviewType::Generic;
}

std::string Preview::detect_mime_type(const std::string& path) const {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".txt" || ext == ".md") return "text/plain";
    if (ext == ".json") return "application/json";
    if (ext == ".xml") return "application/xml";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".zip") return "application/zip";

    return "application/octet-stream";
}

void Preview::load_text_content(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        content_.error = "Cannot open file";
        return;
    }

    std::string line;
    int line_count = 0;
    const int max_lines = 50;

    while (std::getline(file, line) && line_count < max_lines) {
        content_.text_content += line + "\n";
        line_count++;
    }

    if (line_count >= max_lines) {
        content_.text_content += "\n... (truncated)";
    }
}

void Preview::load_image_content(const std::string&) {
    content_.metadata = "Image preview not yet implemented";
}

void Preview::load_code_content(const std::string& path) {
    load_text_content(path);
}

void Preview::load_metadata(const std::string& path) {
    std::ostringstream oss;

    if (file_entry_.is_directory) {
        oss << "Kind: Folder\n";
    } else {
        oss << "Kind: " << FileModel::file_kind_from_path(path) << "\n";
    }

    oss << "Size: " << get_file_size(file_entry_.size) << "\n";
    oss << "Modified: " << get_modification_date(file_entry_.modified_time) << "\n";

    content_.metadata = oss.str();
}

std::string Preview::get_file_size(off_t size) const {
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

std::string Preview::get_modification_date(time_t time) const {
    char buf[64];
    struct tm* tm_info = localtime(&time);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

void Preview::render_text_preview(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    renderer.rounded_rect(x, y, w, h, theme.corner_radius, theme.background);

    if (content_.text_content.empty()) {
        renderer.draw_text(x + 8, y + 8, "No preview available", theme.result_font, theme.text_muted);
        return;
    }

    int line_y = y + 8;
    int line_height = 16;
    std::istringstream stream(content_.text_content);
    std::string line;

    while (std::getline(stream, line) && line_y + line_height < y + h) {
        if (line.length() > 60) {
            line = line.substr(0, 57) + "...";
        }
        renderer.draw_text(x + 8, line_y, line, theme.result_detail_font, theme.foreground);
        line_y += line_height;
    }
}

void Preview::render_metadata(Renderer& renderer, int x, int y, int, const Theme& theme) {
    std::istringstream stream(content_.metadata);
    std::string line;
    int line_y = y;

    while (std::getline(stream, line)) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 2);

            renderer.draw_text(x, line_y, key + ":", theme.result_detail_font, theme.text_muted);
            renderer.draw_text(x + 100, line_y, value, theme.result_detail_font, theme.foreground);
        }
        line_y += 18;
    }
}

} // namespace waylaunch
