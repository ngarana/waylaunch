#pragma once

#include "waylaunch/providers/result_provider.h"

namespace waylaunch {

class AppLauncher;
class HistoryStore;

// Installed applications (over the cached .desktop scan), ranked by name match +
// frecency. Register only when [search].applications is enabled.
class AppProvider : public ResultProvider {
  public:
    AppProvider(const AppLauncher* apps, const HistoryStore* history)
        : apps_(apps), history_(history) {}

    std::string id() const override { return "applications"; }
    bool is_available() const override { return apps_ != nullptr; }
    std::vector<ListItem> query(const ProviderQuery&) override;
    bool activate(const ListItem&) override;

  private:
    const AppLauncher* apps_;
    const HistoryStore* history_;
};

} // namespace waylaunch
