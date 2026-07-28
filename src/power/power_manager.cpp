#include "waylaunch/power/power_manager.h"

namespace waylaunch {

PowerManager::PowerManager(IPowerActionBackend* backend) : backend_(backend) {
    if (backend_) actions_ = backend_->actions();
}

void PowerManager::show() {
    visible_ = true;
    selected_index_ = 0;
    dialog_.close();
    notify_change();
}

void PowerManager::hide() {
    visible_ = false;
    dialog_.close();
    notify_change();
}

void PowerManager::navigate_next() {
    if (actions_.empty()) return;
    selected_index_ = (selected_index_ + 1) % actions_.size();
    notify_change();
}

void PowerManager::navigate_prev() {
    if (actions_.empty()) return;
    selected_index_ = (selected_index_ + actions_.size() - 1) % actions_.size();
    notify_change();
}

void PowerManager::jump_to(size_t index) {
    // Idempotent: pointer motion fires this many times per second over the same
    // card, so re-selecting the current index must not spin the redraw loop.
    if (index < actions_.size() && index != selected_index_) {
        selected_index_ = index;
        notify_change();
    }
}

const PowerAction* PowerManager::selected_action() const {
    if (selected_index_ < actions_.size()) return &actions_[selected_index_];
    return nullptr;
}

void PowerManager::activate_selected() {
    if (!visible_) return;
    const PowerAction* a = selected_action();
    if (!a) return;

    if (a->destructive && confirm_destructive_) {
        dialog_.open(*a, countdown_seconds_);
        notify_change();
        return;
    }
    pending_ = *a;   // runs after the surface is torn down (execute_pending)
    hide();
}

void PowerManager::confirm() {
    if (!dialog_.is_open()) return;   // reject: no action was put up for confirmation
    pending_ = dialog_.action();
    hide();   // also closes the dialog
}

void PowerManager::toggle_dialog_focus() {
    if (!dialog_.is_open()) return;
    dialog_.toggle_focus();
    notify_change();
}

void PowerManager::set_dialog_focus(ConfirmDialog::Button b) {
    if (!dialog_.is_open() || dialog_.focused_button() == b) return;
    dialog_.set_focus(b);
    notify_change();
}

void PowerManager::cancel() {
    // One escape hatch, one meaning: cancelling a confirmation dismisses the
    // whole overlay rather than dropping you back on the picker. "No, don't
    // shut down" means you are done — not that you want to choose something
    // else. hide() closes the dialog too.
    hide();
}

int PowerManager::execute_pending() {
    if (!pending_ || !backend_) return -1;
    int rc = backend_->execute(*pending_);
    pending_.reset();
    return rc;
}

void PowerManager::notify_change() {
    if (on_change_) on_change_();
}

} // namespace waylaunch
