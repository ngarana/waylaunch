#include "waylaunch/dropdown/window_ownership.h"

namespace waylaunch {

Ownership classify_ownership(int client_pid, int owner_pid, const AncestryFn& ancestry) {
    if (owner_pid <= 0 || client_pid <= 0) return Ownership::ClassOnly;
    if (client_pid == owner_pid) return Ownership::Exact;
    if (ancestry && ancestry(client_pid, owner_pid)) return Ownership::Descendant;
    return Ownership::ClassOnly;
}

size_t select_owned(const std::vector<HyprClient>& clients, const std::string& app_id,
                    int owner_pid, const AncestryFn& ancestry) {
    size_t best = std::string::npos;
    Ownership best_tier = Ownership::ClassOnly;
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].klass != app_id) continue;
        Ownership tier = classify_ownership(clients[i].pid, owner_pid, ancestry);
        if (best != std::string::npos && tier >= best_tier) continue;
        best = i;
        best_tier = tier;
        if (tier == Ownership::Exact) break; // cannot do better
    }
    return best;
}

std::vector<size_t> filter_owned(const std::vector<HyprClient>& clients, const std::string& app_id,
                                 int owner_pid, const AncestryFn& ancestry) {
    std::vector<size_t> owned;
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].klass != app_id) continue;
        if (owner_pid > 0 &&
            classify_ownership(clients[i].pid, owner_pid, ancestry) == Ownership::ClassOnly) {
            continue;
        }
        owned.push_back(i);
    }
    return owned;
}

} // namespace waylaunch
