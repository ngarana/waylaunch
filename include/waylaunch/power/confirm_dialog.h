#pragma once

#include "waylaunch/power/power_action.h"
#include <algorithm>
#include <chrono>

namespace waylaunch {

// Value object for the modal confirmation dialog: whether it is shown, which
// action it is about, which button holds focus, and the auto-confirm countdown
// (macOS-style: the dialog executes on its own when the counter reaches zero).
// Owned by PowerManager; rendered by ConfirmDialogRenderer; never touches
// Wayland — header-only by design. Time is injectable for deterministic tests.
class ConfirmDialog {
public:
    using Clock = std::chrono::steady_clock;
    enum class Button { Confirm, Cancel };

    void open(const PowerAction& action, int countdown_seconds = 0,
              Clock::time_point now = Clock::now()) {
        action_ = action;
        open_ = true;
        focus_ = Button::Confirm;   // Return confirms by default (§4.4)
        countdown_total_ = countdown_seconds > 0 ? countdown_seconds : 0;
        deadline_ = now + std::chrono::seconds(countdown_total_);
    }
    void close() { open_ = false; }

    bool is_open() const { return open_; }
    const PowerAction& action() const { return action_; }

    Button focused_button() const { return focus_; }
    void toggle_focus() {
        focus_ = (focus_ == Button::Confirm) ? Button::Cancel : Button::Confirm;
    }

    // --- Countdown (0 total = disabled: never expires, nothing to render) ---
    bool has_countdown() const { return countdown_total_ > 0; }

    int remaining_seconds(Clock::time_point now = Clock::now()) const {
        if (!has_countdown()) return 0;
        auto left = std::chrono::ceil<std::chrono::seconds>(deadline_ - now).count();
        return static_cast<int>(std::max<long long>(0, left));
    }

    // 1 → full time left … 0 → expired. For the depleting ring.
    double remaining_fraction(Clock::time_point now = Clock::now()) const {
        if (!has_countdown()) return 0.0;
        auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
        double f = static_cast<double>(left_ms) / (countdown_total_ * 1000.0);
        return std::clamp(f, 0.0, 1.0);
    }

    bool expired(Clock::time_point now = Clock::now()) const {
        return open_ && has_countdown() && now >= deadline_;
    }

private:
    PowerAction action_{};
    Clock::time_point deadline_{};
    int countdown_total_ = 0;
    Button focus_ = Button::Confirm;
    bool open_ = false;
};

} // namespace waylaunch
