#include "waylaunch/file_ops.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace waylaunch {

FileOperations::FileOperations() {
    const char* home = std::getenv("HOME");
    if (home) {
        trash_directory_ = std::string(home) + "/.local/share/Trash";
    }
}

FileOperations::~FileOperations() = default;

void FileOperations::set_trash_directory(const std::string& path) {
    trash_directory_ = path;
}

const std::string& FileOperations::trash_directory() const {
    return trash_directory_;
}

FileOperationResult FileOperations::copy(const std::string& source, const std::string& destination) {
    FileOperationResult result;
    result.source = source;
    result.destination = destination;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(source)) {
            result.error_message = "Source file does not exist";
            return result;
        }

        fs::path dest_path(destination);
        if (fs::is_directory(source)) {
            if (fs::exists(dest_path)) {
                dest_path = dest_path / fs::path(source).filename();
            }
            fs::copy(source, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } else {
            if (fs::is_directory(dest_path)) {
                dest_path = dest_path / fs::path(source).filename();
            }
            fs::copy(source, dest_path, fs::copy_options::overwrite_existing);
        }

        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error during copy";
    }

    return result;
}

FileOperationResult FileOperations::move(const std::string& source, const std::string& destination) {
    FileOperationResult result;
    result.source = source;
    result.destination = destination;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(source)) {
            result.error_message = "Source file does not exist";
            return result;
        }

        fs::path dest_path(destination);
        if (fs::is_directory(dest_path)) {
            dest_path = dest_path / fs::path(source).filename();
        }

        fs::rename(source, dest_path);
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error during move";
    }

    return result;
}

FileOperationResult FileOperations::rename(const std::string& old_path, const std::string& new_name) {
    FileOperationResult result;
    result.source = old_path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(old_path)) {
            result.error_message = "File does not exist";
            return result;
        }

        fs::path parent = fs::path(old_path).parent_path();
        fs::path new_path = parent / new_name;

        if (fs::exists(new_path)) {
            result.error_message = "A file with that name already exists";
            return result;
        }

        fs::rename(old_path, new_path);
        result.destination = new_path.string();
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error during rename";
    }

    return result;
}

FileOperationResult FileOperations::remove(const std::string& path) {
    return move_to_trash(path);
}

FileOperationResult FileOperations::permanent_delete(const std::string& path) {
    FileOperationResult result;
    result.source = path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(path)) {
            result.error_message = "File does not exist";
            return result;
        }

        if (fs::is_directory(path)) {
            fs::remove_all(path);
        } else {
            fs::remove(path);
        }

        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error during delete";
    }

    return result;
}

FileOperationResult FileOperations::duplicate(const std::string& path) {
    FileOperationResult result;
    result.source = path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(path)) {
            result.error_message = "File does not exist";
            return result;
        }

        fs::path source_path(path);
        fs::path parent = source_path.parent_path();
        std::string stem = source_path.stem().string();
        std::string extension = source_path.extension().string();

        std::string new_name = generate_unique_name(parent.string(), stem + " copy" + extension);
        fs::path dest_path = parent / new_name;

        if (fs::is_directory(path)) {
            fs::copy(path, dest_path, fs::copy_options::recursive);
        } else {
            fs::copy(path, dest_path);
        }

        result.destination = dest_path.string();
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error during duplicate";
    }

    return result;
}

FileOperationResult FileOperations::new_folder(const std::string& parent_path, const std::string& name) {
    FileOperationResult result;
    result.source = parent_path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(parent_path) || !fs::is_directory(parent_path)) {
            result.error_message = "Parent directory does not exist";
            return result;
        }

        std::string folder_name = name.empty() ? "New Folder" : name;
        fs::path new_path = fs::path(parent_path) / folder_name;

        int counter = 1;
        while (fs::exists(new_path)) {
            new_path = fs::path(parent_path) / (folder_name + " " + std::to_string(counter));
            counter++;
        }

        fs::create_directory(new_path);
        result.destination = new_path.string();
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error creating folder";
    }

    return result;
}

FileOperationResult FileOperations::move_to_trash(const std::string& path) {
    FileOperationResult result;
    result.source = path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(path)) {
            result.error_message = "File does not exist";
            return result;
        }

        ensure_trash_structure();

        fs::path source_path(path);
        std::string file_name = source_path.filename().string();
        std::string trash_path = get_trash_files_path(file_name);

        int counter = 1;
        while (fs::exists(trash_path)) {
            std::string ext = source_path.extension().string();
            std::string stem = source_path.stem().string();
            trash_path = get_trash_files_path(stem + " " + std::to_string(counter) + ext);
            counter++;
        }

        // Try atomic rename first; fall back to copy+delete across filesystems.
        try {
            fs::rename(path, trash_path);
        } catch (const std::filesystem::filesystem_error&) {
            if (fs::is_directory(path)) {
                fs::copy(path, trash_path, fs::copy_options::recursive);
                fs::remove_all(path);
            } else {
                fs::copy(path, trash_path);
                fs::remove(path);
            }
        }
        write_trash_info(fs::path(trash_path).filename().string(), path);

        result.destination = trash_path;
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error moving to trash";
    }

    return result;
}

FileOperationResult FileOperations::restore_from_trash(const std::string& trash_path, const std::string& original_path) {
    FileOperationResult result;
    result.source = trash_path;
    result.destination = original_path;

    try {
        namespace fs = std::filesystem;

        if (!fs::exists(trash_path)) {
            result.error_message = "File not found in trash";
            return result;
        }

        fs::path dest(original_path);
        if (fs::exists(dest)) {
            std::string stem = dest.stem().string();
            std::string ext = dest.extension().string();
            int counter = 1;
            while (fs::exists(dest)) {
                dest = dest.parent_path() / (stem + " " + std::to_string(counter) + ext);
                counter++;
            }
            result.destination = dest.string();
        }

        fs::rename(trash_path, dest);
        remove_trash_info(fs::path(trash_path).filename().string());
        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error restoring from trash";
    }

    return result;
}

FileOperationResult FileOperations::empty_trash() {
    FileOperationResult result;

    try {
        namespace fs = std::filesystem;

        std::string files_path = trash_directory_ + "/files";
        std::string info_path = trash_directory_ + "/info";

        if (fs::exists(files_path)) {
            fs::remove_all(files_path);
            fs::create_directory(files_path);
        }

        if (fs::exists(info_path)) {
            fs::remove_all(info_path);
            fs::create_directory(info_path);
        }

        result.success = true;
    } catch (const std::filesystem::filesystem_error& e) {
        result.error_message = e.what();
    } catch (...) {
        result.error_message = "Unknown error emptying trash";
    }

    return result;
}

std::vector<std::string> FileOperations::list_trash_contents() const {
    std::vector<std::string> contents;

    namespace fs = std::filesystem;
    std::string files_path = trash_directory_ + "/files";

    if (!fs::exists(files_path)) return contents;

    for (const auto& entry : fs::directory_iterator(files_path)) {
        contents.push_back(entry.path().string());
    }

    std::sort(contents.begin(), contents.end());
    return contents;
}

bool FileOperations::create_directory(const std::string& path) {
    try {
        return std::filesystem::create_directories(path);
    } catch (...) {
        return false;
    }
}

bool FileOperations::file_exists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool FileOperations::is_directory(const std::string& path) {
    try {
        return std::filesystem::is_directory(path);
    } catch (...) {
        return false;
    }
}

std::string FileOperations::generate_unique_name(const std::string& directory, const std::string& base_name) {
    namespace fs = std::filesystem;

    std::string name = base_name;
    fs::path full_path = fs::path(directory) / name;

    int counter = 1;
    while (fs::exists(full_path)) {
        std::string ext = fs::path(base_name).extension().string();
        std::string stem = fs::path(base_name).stem().string();
        name = stem + " " + std::to_string(counter) + ext;
        full_path = fs::path(directory) / name;
        counter++;
    }

    return name;
}

void FileOperations::copy_async(const std::string& source, const std::string& destination, FileOperationCallback callback) {
    std::thread([this, source, destination, callback]() {
        auto result = copy(source, destination);
        if (callback) callback(result);
    }).detach();
}

void FileOperations::move_async(const std::string& source, const std::string& destination, FileOperationCallback callback) {
    std::thread([this, source, destination, callback]() {
        auto result = move(source, destination);
        if (callback) callback(result);
    }).detach();
}

void FileOperations::remove_async(const std::string& path, FileOperationCallback callback) {
    std::thread([this, path, callback]() {
        auto result = remove(path);
        if (callback) callback(result);
    }).detach();
}

void FileOperations::ensure_trash_structure() {
    namespace fs = std::filesystem;

    if (!fs::exists(trash_directory_)) {
        fs::create_directories(trash_directory_);
    }

    std::string files_path = trash_directory_ + "/files";
    std::string info_path = trash_directory_ + "/info";

    if (!fs::exists(files_path)) fs::create_directory(files_path);
    if (!fs::exists(info_path)) fs::create_directory(info_path);
}

std::string FileOperations::get_trash_info_path(const std::string& file_name) {
    return trash_directory_ + "/info/" + file_name + ".trashinfo";
}

std::string FileOperations::get_trash_files_path(const std::string& file_name) {
    return trash_directory_ + "/files/" + file_name;
}

bool FileOperations::write_trash_info(const std::string& file_name, const std::string& original_path) {
    std::string info_path = get_trash_info_path(file_name);
    std::ofstream file(info_path);
    if (!file.is_open()) return false;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    file << "[Trash Info]\n";
    file << "Path=" << original_path << "\n";
    file << "DeletionDate=" << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S") << "\n";

    return file.good();
}

bool FileOperations::remove_trash_info(const std::string& file_name) {
    std::string info_path = get_trash_info_path(file_name);
    return std::filesystem::remove(info_path);
}

} // namespace waylaunch
