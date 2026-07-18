#include "waylaunch/file_model.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <map>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

namespace waylaunch {

std::string FileEntry::display_name() const {
    return name;
}

std::string FileEntry::display_size() const {
    if (is_directory) return "--";

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

std::string FileEntry::display_date() const {
    char buf[64];
    struct tm* tm_info = localtime(&modified_time);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
    return buf;
}

std::string FileEntry::display_permissions() const {
    std::string perms;
    perms += (permissions & S_IRUSR) ? 'r' : '-';
    perms += (permissions & S_IWUSR) ? 'w' : '-';
    perms += (permissions & S_IXUSR) ? 'x' : '-';
    perms += (permissions & S_IRGRP) ? 'r' : '-';
    perms += (permissions & S_IWGRP) ? 'w' : '-';
    perms += (permissions & S_IXGRP) ? 'x' : '-';
    perms += (permissions & S_IROTH) ? 'r' : '-';
    perms += (permissions & S_IWOTH) ? 'w' : '-';
    perms += (permissions & S_IXOTH) ? 'x' : '-';
    return perms;
}

std::string FileEntry::file_kind() const {
    return FileModel::file_kind_from_path(path);
}

FileModel::FileModel() = default;
FileModel::~FileModel() = default;

void FileModel::set_directory(const std::string& path) {
    std::string resolved = resolve_path(path);
    if (resolved != current_directory_) {
        current_directory_ = resolved;
        nav_state_.history.clear();
        nav_state_.history.push_back(current_directory_);
        nav_state_.history_index = 0;
        update_navigation();
        load_directory();
    }
}

const std::string& FileModel::current_directory() const {
    return current_directory_;
}

void FileModel::navigate_to(const std::string& path) {
    std::string resolved = resolve_path(path);
    struct stat st;
    if (stat(resolved.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;

    if (nav_state_.history_index >= 0 && nav_state_.history_index < static_cast<int>(nav_state_.history.size()) - 1) {
        nav_state_.history.resize(nav_state_.history_index + 1);
    }

    if (nav_state_.history.empty() || nav_state_.history.back() != resolved) {
        nav_state_.history.push_back(resolved);
        if (static_cast<int>(nav_state_.history.size()) > 100) {
            nav_state_.history.erase(nav_state_.history.begin());
        }
    }
    nav_state_.history_index = static_cast<int>(nav_state_.history.size()) - 1;

    current_directory_ = resolved;
    update_navigation();
    load_directory();
}

void FileModel::go_back() {
    if (nav_state_.can_go_back) {
        nav_state_.history_index--;
        current_directory_ = nav_state_.history[nav_state_.history_index];
        update_navigation();
        load_directory();
    }
}

void FileModel::go_forward() {
    if (nav_state_.can_go_forward) {
        nav_state_.history_index++;
        current_directory_ = nav_state_.history[nav_state_.history_index];
        update_navigation();
        load_directory();
    }
}

void FileModel::go_up() {
    namespace fs = std::filesystem;
    fs::path parent = fs::path(current_directory_).parent_path();
    if (parent != current_directory_) {
        navigate_to(parent.string());
    }
}

void FileModel::go_home() {
    navigate_to(home_directory());
}

void FileModel::refresh() {
    load_directory();
}

const NavigationState& FileModel::navigation_state() const {
    return nav_state_;
}

void FileModel::set_view_mode(ViewMode mode) {
    view_mode_ = mode;
}

ViewMode FileModel::view_mode() const {
    return view_mode_;
}

void FileModel::set_sort_field(SortField field) {
    if (sort_field_ != field) {
        sort_field_ = field;
        sort_entries();
    }
}

SortField FileModel::sort_field() const {
    return sort_field_;
}

void FileModel::set_sort_order(SortOrder order) {
    if (sort_order_ != order) {
        sort_order_ = order;
        sort_entries();
    }
}

SortOrder FileModel::sort_order() const {
    return sort_order_;
}

void FileModel::set_show_hidden(bool show) {
    if (show_hidden_ != show) {
        show_hidden_ = show;
        load_directory();
    }
}

bool FileModel::show_hidden() const {
    return show_hidden_;
}

const std::vector<FileEntry>& FileModel::entries() const {
    return entries_;
}

const std::vector<FileEntry>& FileModel::sorted_entries() const {
    return sorted_entries_;
}

std::vector<FileEntry> FileModel::get_selected_entries() const {
    std::vector<FileEntry> result;
    for (int idx : selected_indices_) {
        if (idx >= 0 && idx < static_cast<int>(sorted_entries_.size())) {
            result.push_back(sorted_entries_[idx]);
        }
    }
    return result;
}

void FileModel::select_entry(int index) {
    if (index >= 0 && index < static_cast<int>(sorted_entries_.size())) {
        selected_indices_.clear();
        selected_indices_.push_back(index);
        selected_index_ = index;
    }
}

void FileModel::select_entry_range(int start, int end) {
    selected_indices_.clear();
    int s = std::max(0, std::min(start, end));
    int e = std::min(static_cast<int>(sorted_entries_.size()) - 1, std::max(start, end));
    for (int i = s; i <= e; i++) {
        selected_indices_.push_back(i);
    }
    selected_index_ = s;
}

void FileModel::select_all() {
    selected_indices_.clear();
    for (int i = 0; i < static_cast<int>(sorted_entries_.size()); i++) {
        selected_indices_.push_back(i);
    }
    selected_index_ = sorted_entries_.empty() ? -1 : 0;
}

void FileModel::deselect_all() {
    selected_indices_.clear();
    selected_index_ = -1;
}

int FileModel::selected_index() const {
    return selected_index_;
}

const std::vector<int>& FileModel::selected_indices() const {
    return selected_indices_;
}

bool FileModel::is_selected(int index) const {
    return std::find(selected_indices_.begin(), selected_indices_.end(), index) != selected_indices_.end();
}

std::vector<std::string> FileModel::get_favorites() const {
    return favorites_;
}

void FileModel::add_favorite(const std::string& path) {
    if (std::find(favorites_.begin(), favorites_.end(), path) == favorites_.end()) {
        favorites_.push_back(path);
    }
}

void FileModel::remove_favorite(const std::string& path) {
    favorites_.erase(std::remove(favorites_.begin(), favorites_.end(), path), favorites_.end());
}

std::string FileModel::home_directory() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : "/root";
}

std::string FileModel::desktop_directory() {
    return home_directory() + "/Desktop";
}

std::string FileModel::documents_directory() {
    return home_directory() + "/Documents";
}

std::string FileModel::downloads_directory() {
    return home_directory() + "/Downloads";
}

std::vector<std::string> FileModel::default_favorites() {
    std::vector<std::string> favs;
    favs.push_back(home_directory());
    favs.push_back(desktop_directory());
    favs.push_back(documents_directory());
    favs.push_back(downloads_directory());

    const char* music = std::getenv("XDG_MUSIC_DIR");
    if (music) favs.push_back(music);

    const char* pictures = std::getenv("XDG_PICTURES_DIR");
    if (pictures) favs.push_back(pictures);

    const char* videos = std::getenv("XDG_VIDEOS_DIR");
    if (videos) favs.push_back(videos);

    return favs;
}

std::string FileModel::file_kind_from_extension(const std::string& ext) {
    static const std::map<std::string, std::string> kinds = {
        {".txt", "Text Document"},
        {".md", "Markdown Document"},
        {".pdf", "PDF Document"},
        {".doc", "Word Document"},
        {".docx", "Word Document"},
        {".xls", "Spreadsheet"},
        {".xlsx", "Spreadsheet"},
        {".ppt", "Presentation"},
        {".pptx", "Presentation"},
        {".jpg", "JPEG Image"},
        {".jpeg", "JPEG Image"},
        {".png", "PNG Image"},
        {".gif", "GIF Image"},
        {".svg", "SVG Image"},
        {".webp", "WebP Image"},
        {".bmp", "Bitmap Image"},
        {".mp3", "MP3 Audio"},
        {".wav", "WAV Audio"},
        {".flac", "FLAC Audio"},
        {".ogg", "OGG Audio"},
        {".mp4", "MP4 Video"},
        {".mkv", "MKV Video"},
        {".avi", "AVI Video"},
        {".mov", "MOV Video"},
        {".zip", "ZIP Archive"},
        {".tar", "TAR Archive"},
        {".gz", "GZIP Archive"},
        {".bz2", "BZIP2 Archive"},
        {".xz", "XZ Archive"},
        {".7z", "7-Zip Archive"},
        {".c", "C Source"},
        {".cpp", "C++ Source"},
        {".h", "C/C++ Header"},
        {".hpp", "C++ Header"},
        {".py", "Python Script"},
        {".js", "JavaScript"},
        {".ts", "TypeScript"},
        {".html", "HTML Document"},
        {".css", "CSS Stylesheet"},
        {".json", "JSON File"},
        {".xml", "XML File"},
        {".yaml", "YAML File"},
        {".yml", "YAML File"},
        {".toml", "TOML File"},
        {".rs", "Rust Source"},
        {".go", "Go Source"},
        {".java", "Java Source"},
        {".sh", "Shell Script"},
        {".bash", "Bash Script"},
        {".zsh", "Zsh Script"},
        {".conf", "Configuration File"},
        {".log", "Log File"},
        {".ini", "INI File"},
        {".desktop", "Desktop Entry"},
    };

    auto it = kinds.find(ext);
    if (it != kinds.end()) return it->second;
    return ext.empty() ? "File" : ext.substr(1) + " File";
}

std::string FileModel::file_kind_from_path(const std::string& path) {
    namespace fs = std::filesystem;
    try {
        if (fs::is_directory(path)) return "Folder";
        if (fs::is_symlink(path)) return "Symbolic Link";
    } catch (...) {}
    return file_kind_from_extension(fs::path(path).extension().string());
}

void FileModel::load_directory() {
    entries_.clear();
    selected_indices_.clear();
    selected_index_ = -1;

    DIR* dir = opendir(current_directory_.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!show_hidden_ && !name.empty() && name[0] == '.') continue;

        std::string full_path = current_directory_;
        if (full_path.back() != '/') full_path += '/';
        full_path += name;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) continue;

        FileEntry fe;
        fe.name = name;
        fe.path = full_path;
        fe.is_directory = S_ISDIR(st.st_mode);
        fe.is_symlink = S_ISLNK(st.st_mode);
        fe.is_hidden = !name.empty() && name[0] == '.';
        fe.size = st.st_size;
        fe.modified_time = st.st_mtime;
        fe.created_time = st.st_ctime;
        fe.accessed_time = st.st_atime;
        fe.permissions = st.st_mode & 0777;
        fe.uid = st.st_uid;
        fe.gid = st.st_gid;

        namespace fs = std::filesystem;
        fe.extension = fs::path(name).extension().string();

        entries_.push_back(std::move(fe));
    }
    closedir(dir);

    sort_entries();
}

void FileModel::update_navigation() {
    namespace fs = std::filesystem;
    fs::path parent = fs::path(current_directory_).parent_path();
    nav_state_.can_go_up = (parent != current_directory_);
    nav_state_.can_go_back = nav_state_.history_index > 0;
    nav_state_.can_go_forward = nav_state_.history_index < static_cast<int>(nav_state_.history.size()) - 1;
    nav_state_.current_path = current_directory_;
}

void FileModel::sort_entries() {
    sorted_entries_ = entries_;

    std::stable_sort(sorted_entries_.begin(), sorted_entries_.end(),
        [this](const FileEntry& a, const FileEntry& b) {
            int cmp = 0;
            switch (sort_field_) {
                case SortField::Name:
                    cmp = strcasecmp(a.name.c_str(), b.name.c_str());
                    break;
                case SortField::DateModified:
                    cmp = (a.modified_time < b.modified_time) ? -1 :
                          (a.modified_time > b.modified_time) ? 1 : 0;
                    break;
                case SortField::DateCreated:
                    cmp = (a.created_time < b.created_time) ? -1 :
                          (a.created_time > b.created_time) ? 1 : 0;
                    break;
                case SortField::Size:
                    cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
                    break;
                case SortField::Kind: {
                    std::string ka = FileModel::file_kind_from_path(a.path);
                    std::string kb = FileModel::file_kind_from_path(b.path);
                    cmp = strcasecmp(ka.c_str(), kb.c_str());
                    break;
                }
                case SortField::Tags:
                    cmp = (a.tags.size() < b.tags.size()) ? -1 :
                          (a.tags.size() > b.tags.size()) ? 1 : 0;
                    break;
            }
            return (sort_order_ == SortOrder::Ascending) ? (cmp < 0) : (cmp > 0);
        });
}

std::string FileModel::resolve_path(const std::string& path) const {
    namespace fs = std::filesystem;
    try {
        fs::path p(path);
        if (p.is_relative()) {
            p = fs::path(home_directory()) / p;
        }
        return fs::canonical(p).string();
    } catch (...) {
        return path;
    }
}

} // namespace waylaunch
