#pragma once

#include "waylaunch/power/power_action.h"
#include <string>
#include <vector>

namespace waylaunch {

struct PowerConfig;

// Dependency-inversion seam (≈ IToplevelBackend for the switcher): PowerManager
// talks to this interface only, so tests drive it with a stub and never spawn
// real commands.
class IPowerActionBackend {
public:
    virtual ~IPowerActionBackend() = default;
    virtual const std::vector<PowerAction>& actions() const = 0;
    // Run the action's command (fork+exec via Subprocess — no shell). Returns
    // the command's exit code, or -1 if nothing could be executed.
    virtual int execute(const PowerAction& action) = 0;
};

// Concrete backend: builds the six default actions, then applies the [power]
// config — ordering/filtering via enabled_actions, per-action command and
// confirm-text overrides.
class PowerActionBackend : public IPowerActionBackend {
public:
    explicit PowerActionBackend(const PowerConfig& cfg);

    const std::vector<PowerAction>& actions() const override { return actions_; }
    int execute(const PowerAction& action) override;

    // The argv that execute() would run for this action *right now*: applies
    // the exit fallback and the systemd/elogind power-verb normalization —
    // separated from execute() so tests can verify it without running anything.
    static std::vector<std::string> resolve_argv(const PowerAction& action);

    // Whitespace argv splitter honouring '...' and "..." quoting. Public and
    // static so the override-merging tests can exercise it directly.
    static std::vector<std::string> split_argv(const std::string& command);

private:
    std::vector<PowerAction> actions_;
};

} // namespace waylaunch
