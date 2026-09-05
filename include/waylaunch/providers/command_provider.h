#pragma once

#include "waylaunch/providers/result_provider.h"
#include <vector>

namespace waylaunch {

struct Command;
class HistoryStore;

// Custom [[commands]] entries, matched by name; Return runs the command via
// /bin/sh. Register only when [search].commands is enabled.
class CommandProvider : public ResultProvider {
  public:
    CommandProvider(const std::vector<Command>* commands, const HistoryStore* history)
        : commands_(commands), history_(history) {}

    std::string id() const override { return "commands"; }
    std::vector<ListItem> query(const ProviderQuery&) override;
    bool activate(const ListItem&) override;

  private:
    const std::vector<Command>* commands_;
    const HistoryStore* history_;
};

} // namespace waylaunch
