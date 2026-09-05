#include "waylaunch/search_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace waylaunch {

namespace {
std::string to_lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string(".");
}

std::string abbreviate_home(const std::string& path) {
    std::string h = home_dir();
    if (!h.empty() && path.starts_with(h)) return "~" + path.substr(h.size());
    return path;
}

std::string icon_for_file(const std::string& path) {
    std::string ext = to_lower(std::filesystem::path(path).extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".svg" ||
        ext == ".webp" || ext == ".bmp")
        return "image-x-generic";
    if (ext == ".pdf") return "application-pdf";
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a")
        return "audio-x-generic";
    if (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".mov") return "video-x-generic";
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".7z" ||
        ext == ".rar")
        return "package-x-generic";
    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" || ext == ".py" ||
        ext == ".js" || ext == ".ts" || ext == ".rs" || ext == ".go" || ext == ".java" ||
        ext == ".sh")
        return "text-x-script";
    return "text-x-generic";
}

int path_depth(const std::string& p) {
    return static_cast<int>(std::count(p.begin(), p.end(), '/'));
}

float recency_bonus(std::time_t mtime) {
    double days = static_cast<double>(std::time(nullptr) - mtime) / 86400.0;
    if (days < 1) return 60.0F;
    if (days < 7) return 40.0F;
    if (days < 30) return 20.0F;
    if (days < 365) return 8.0F;
    return 0.0F;
}

} // namespace waylaunch
