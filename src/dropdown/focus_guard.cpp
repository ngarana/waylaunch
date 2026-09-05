#include "waylaunch/dropdown/focus_guard.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace waylaunch {
namespace {

// /proc/<pid>/stat looks like `1234 (co-mm and (parens)) S 1000 ...`:
// the comm field may contain spaces and parens, so the ppid scan starts
// after the LAST ')'. Returns -1 when unparseable.
int parent_of(int pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat.is_open()) return -1;
    std::string contents;
    std::getline(stat, contents);
    size_t close = contents.rfind(')');
    if (close == std::string::npos || close + 4 > contents.size()) return -1;
    // After ')': " <state> <ppid> ...". Walk past them explicitly so a
    // malformed line fails instead of converting garbage.
    const char* rest = contents.c_str() + close + 1;
    while (*rest == ' ') ++rest;
    if (*rest == '\0') return -1;
    ++rest; // state character
    char* stop = nullptr;
    long ppid = std::strtol(rest, &stop, 10);
    if (stop == rest || ppid <= 0 || ppid > 4194304) return -1;
    return static_cast<int>(ppid);
}

} // namespace

std::string normalize_address(const std::string& address) {
    std::string out;
    out.reserve(address.size());
    size_t start = 0;
    if (address.size() > 2 && address[0] == '0' && (address[1] == 'x' || address[1] == 'X')) {
        start = 2;
    }
    for (size_t i = start; i < address.size(); ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(address[i]))));
    }
    return out;
}

bool is_descendant_process(int pid, int ancestor) {
    if (pid == ancestor) return true;
    // Bounded walk: a cycle or a missing /proc entry ends in false.
    for (int hops = 0; hops < 64; ++hops) {
        if (pid <= 1) return false;
        int parent = parent_of(pid);
        if (parent < 0) return false;
        if (parent == ancestor) return true;
        pid = parent;
    }
    return false;
}

void FocusGuard::configure(bool hide_on_focus_loss, int grace_ms, AncestryFn ancestry) {
    hide_on_focus_loss_ = hide_on_focus_loss;
    grace_ms_ = grace_ms;
    ancestry_ = std::move(ancestry);
}

void FocusGuard::set_slot(pid_t pid, const std::string& address) {
    slot_pid_ = pid;
    slot_address_ = normalize_address(address);
}

void FocusGuard::note_shown(std::chrono::steady_clock::time_point now) {
    has_shown_ = true;
    last_shown_ = now;
}

bool FocusGuard::should_retract(std::chrono::steady_clock::time_point now,
                                const std::string& focused_address, int focused_pid) {
    if (!hide_on_focus_loss_) return false;
    if (!slot_address_.empty() && normalize_address(focused_address) == slot_address_) {
        return false; // focus is still ours
    }
    if (has_shown_ && now - last_shown_ < std::chrono::milliseconds(grace_ms_)) {
        return false; // our own show's focus events
    }
    if (focused_pid >= 0 && slot_pid_ >= 0 && ancestry_(focused_pid, slot_pid_)) {
        return false; // a menu/picker owned by the terminal
    }
    return true;
}

} // namespace waylaunch
