#include "waylaunch/dropdown/dropdown_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace waylaunch {
namespace {

// TSV row: slot\twidth\theight. Slot names come from the user's config;
// anything outside [name, int, int] is skipped rather than trusted.
bool parse_row(const std::string& line, std::string& slot, DropdownSlotState& state) {
    size_t first = line.find('\t');
    size_t second = line.find('\t', first == std::string::npos ? 0 : first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;
    slot = line.substr(0, first);
    if (slot.empty() || slot.find('\n') != std::string::npos) return false;
    try {
        size_t used = 0;
        int w = std::stoi(line.substr(first + 1, second - first - 1), &used);
        if (used == 0) return false;
        std::string tail = line.substr(second + 1);
        size_t hused = 0;
        int h = std::stoi(tail, &hused);
        if (hused != tail.size() || tail.empty()) return false;
        if (w <= 0 || h <= 0) return false;
        state = DropdownSlotState{.w = w, .h = h};
        return true;
    } catch (const std::exception&) { return false; }
}

} // namespace

std::string DropdownStateStore::default_path() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] != '\0') {
        return (std::filesystem::path(state) / "waylaunch/dropdown.tsv").string();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return (std::filesystem::path(home) / ".local/state/waylaunch/dropdown.tsv").string();
    }
    return ".waylaunch-dropdown.tsv";
}

std::map<std::string, DropdownSlotState> DropdownStateStore::load() const {
    std::map<std::string, DropdownSlotState> states;
    std::ifstream file(path_);
    if (!file.is_open()) return states;
    std::string line;
    while (std::getline(file, line)) {
        std::string slot;
        DropdownSlotState state;
        if (parse_row(line, slot, state)) states[slot] = state;
    }
    return states;
}

bool DropdownStateStore::save_slot(const std::string& slot, const DropdownSlotState& state) {
    if (slot.empty() || slot.find_first_of("\t\n") != std::string::npos) return false;
    if (state.w <= 0 || state.h <= 0) return false;
    auto states = load();
    states[slot] = state;
    return save_all(states);
}

bool DropdownStateStore::save_all(const std::map<std::string, DropdownSlotState>& states) const {
    if (path_.empty()) return false;
    std::error_code error;
    std::filesystem::path path(path_);
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    const std::string temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file.is_open()) return false;
        for (const auto& [slot, state] : states) {
            file << slot << '\t' << state.w << '\t' << state.h << '\n';
        }
        file.close();
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return !error;
}

} // namespace waylaunch
