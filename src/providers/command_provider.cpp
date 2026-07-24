#include "waylaunch/providers/command_provider.h"
#include "waylaunch/config.h"
#include "waylaunch/history.h"
#include "waylaunch/subprocess.h"
#include <algorithm>

namespace waylaunch {

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
} // namespace

std::vector<ListItem> CommandProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    if (!commands_) return out;
    for (const auto& cmd : *commands_) {
        if (cmd.name.empty() || cmd.command.empty()) continue;
        std::string n = to_lower(cmd.name);
        size_t pos = n.find(q.lower);
        if (pos == std::string::npos) continue;
        ListItem it;
        it.kind = ItemKind::Command;
        it.name = cmd.name;
        it.action_command = cmd.command;
        it.icon_name = cmd.icon.empty() ? "utilities-terminal" : cmd.icon;
        it.description = cmd.category.empty() ? "Command" : cmd.category;
        it.score = (pos == 0 ? 900.0f : 400.0f) - std::min<size_t>(pos, 200);
        if (history_) it.score += history_->frecency("command:" + cmd.command);
        out.push_back(std::move(it));
    }
    return out;
}

bool CommandProvider::activate(const ListItem& it) {
    if (it.kind != ItemKind::Command) return false;
    if (!it.action_command.empty())
        Subprocess::spawn_detached({"/bin/sh", "-c", it.action_command});
    return true;
}

} // namespace waylaunch
