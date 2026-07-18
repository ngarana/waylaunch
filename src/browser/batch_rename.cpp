#include "waylaunch/batch_rename.h"
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <cctype>

namespace waylaunch {

BatchRename::BatchRename() = default;
BatchRename::~BatchRename() = default;

void BatchRename::set_visible(bool visible) { visible_ = visible; }
bool BatchRename::is_visible() const { return visible_; }

void BatchRename::set_files(const std::vector<std::string>& file_paths) {
    files_ = file_paths;
    update_preview();
}

const std::vector<std::string>& BatchRename::files() const { return files_; }

void BatchRename::set_mode(RenameMode mode) {
    mode_ = mode;
    update_preview();
}

RenameMode BatchRename::mode() const { return mode_; }

void BatchRename::set_find_text(const std::string& text) { find_text_ = text; update_preview(); }
void BatchRename::set_replace_text(const std::string& text) { replace_text_ = text; update_preview(); }
void BatchRename::set_prefix(const std::string& prefix) { prefix_ = prefix; update_preview(); }
void BatchRename::set_suffix(const std::string& suffix) { suffix_ = suffix; update_preview(); }
void BatchRename::set_start_number(int number) { start_number_ = number; update_preview(); }
void BatchRename::set_digits(int digits) { digits_ = digits; update_preview(); }

void BatchRename::update_preview() {
    preview_.clear();

    for (size_t i = 0; i < files_.size(); i++) {
        RenameEntry entry;
        entry.original_name = std::filesystem::path(files_[i]).filename().string();
        entry.new_name = apply_rename(entry.original_name, static_cast<int>(i));

        if (entry.new_name == entry.original_name) {
            entry.valid = false;
        }

        preview_.push_back(std::move(entry));
    }
}

const std::vector<RenameEntry>& BatchRename::preview() const { return preview_; }

bool BatchRename::has_errors() const {
    return std::any_of(preview_.begin(), preview_.end(),
        [](const RenameEntry& e) { return !e.error.empty(); });
}

std::string BatchRename::apply_rename(const std::string& original_name, int index) const {
    namespace fs = std::filesystem;

    std::string stem = fs::path(original_name).stem().string();
    std::string ext = fs::path(original_name).extension().string();

    switch (mode_) {
        case RenameMode::FindReplace: {
            if (find_text_.empty()) return original_name;
            std::string result = stem;
            size_t pos = 0;
            while ((pos = result.find(find_text_, pos)) != std::string::npos) {
                result.replace(pos, find_text_.length(), replace_text_);
                pos += replace_text_.length();
            }
            return result + ext;
        }

        case RenameMode::AddPrefix:
            return prefix_ + stem + ext;

        case RenameMode::AddSuffix:
            return stem + suffix_ + ext;

        case RenameMode::Sequential:
            return sequential_name(stem, start_number_ + index, digits_) + ext;

        case RenameMode::Lowercase: {
            std::string result = stem;
            std::transform(result.begin(), result.end(), result.begin(), ::tolower);
            return result + ext;
        }

        case RenameMode::Uppercase: {
            std::string result = stem;
            std::transform(result.begin(), result.end(), result.begin(), ::toupper);
            return result + ext;
        }

        case RenameMode::ReplaceSpaces: {
            std::string result = stem;
            std::replace(result.begin(), result.end(), ' ', '_');
            return result + ext;
        }
    }

    return original_name;
}

std::string BatchRename::sequential_name(const std::string&, int number, int digits) const {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(digits) << number;
    return oss.str();
}

void BatchRename::render(Renderer& renderer, int x, int y, int w, int h, const Theme& theme) {
    if (!visible_) return;

    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int current_y = y + PADDING;

    std::vector<TextSegment> title;
    title.push_back({"Batch Rename", theme.foreground});
    renderer.draw_text_segments(x + PADDING, current_y, title, theme.result_font);
    current_y += 24 + 8;

    const char* mode_names[] = {"Find & Replace", "Add Prefix", "Add Suffix", "Sequential", "Lowercase", "Uppercase", "Replace Spaces"};
    renderer.draw_text(x + PADDING, current_y, "Mode:", theme.result_detail_font, theme.text_muted);
    renderer.draw_text(x + PADDING + 50, current_y, mode_names[static_cast<int>(mode_)], theme.result_font, theme.foreground);
    current_y += ITEM_HEIGHT + 8;

    int preview_h = std::min(static_cast<int>(preview_.size()), 8) * ITEM_HEIGHT;
    renderer.rounded_rect(x + PADDING, current_y, w - 2 * PADDING, preview_h + 8, theme.corner_radius, theme.background);

    int py = current_y + 4;
    for (const auto& entry : preview_) {
        if (py + ITEM_HEIGHT > current_y + preview_h + 8) break;

        std::vector<TextSegment> segments;
        segments.push_back({entry.original_name + " -> ", theme.text_muted});
        segments.push_back({entry.new_name, entry.valid ? theme.foreground : theme.error});
        renderer.draw_text_segments(x + PADDING + 8, py + 4, segments, theme.result_detail_font);
        py += ITEM_HEIGHT;
    }
}

void BatchRename::set_rename_callback(RenameCallback callback) { rename_callback_ = callback; }
void BatchRename::set_cancel_callback(CancelCallback callback) { cancel_callback_ = callback; }

} // namespace waylaunch
