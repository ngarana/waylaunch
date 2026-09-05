#pragma once

#include "waylaunch/providers/result_provider.h"

namespace waylaunch {

// A valid math expression becomes the Top Hit; Return copies the result to the
// clipboard. Register only when [search].calculator is enabled.
class CalculatorProvider : public ResultProvider {
  public:
    std::string id() const override { return "calculator"; }
    std::vector<ListItem> query(const ProviderQuery&) override;
    bool activate(const ListItem&) override;
};

} // namespace waylaunch
