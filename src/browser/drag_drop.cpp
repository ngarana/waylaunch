#include "waylaunch/drag_drop.h"
#include <algorithm>

namespace waylaunch {

DragDrop::DragDrop() = default;
DragDrop::~DragDrop() = default;

void DragDrop::start_drag(const std::vector<std::string>& paths, double x, double y, DragAction action) {
    data_.paths = paths;
    data_.action = action;
    data_.is_active = true;
    data_.start_x = x;
    data_.start_y = y;
    data_.current_x = x;
    data_.current_y = y;

    if (drag_started_callback_) {
        drag_started_callback_();
    }
}

void DragDrop::update_drag(double x, double y) {
    if (!data_.is_active) return;

    data_.current_x = x;
    data_.current_y = y;
}

void DragDrop::end_drag() {
    if (!data_.is_active) return;

    if (!drop_target_.empty() && !data_.paths.empty()) {
        if (drop_callback_) {
            drop_callback_(data_.paths, drop_target_, data_.action);
        }
    }

    data_.is_active = false;
    data_.paths.clear();
    drop_target_.clear();

    if (drag_ended_callback_) {
        drag_ended_callback_();
    }
}

void DragDrop::cancel_drag() {
    data_.is_active = false;
    data_.paths.clear();
    drop_target_.clear();

    if (drag_ended_callback_) {
        drag_ended_callback_();
    }
}

bool DragDrop::is_dragging() const {
    return data_.is_active;
}

const DragData& DragDrop::drag_data() const {
    return data_;
}

void DragDrop::set_drop_target(const std::string& path) {
    drop_target_ = path;
}

const std::string& DragDrop::drop_target() const {
    return drop_target_;
}

void DragDrop::set_drop_callback(DropCallback callback) {
    drop_callback_ = callback;
}

void DragDrop::set_drag_started_callback(DragStartedCallback callback) {
    drag_started_callback_ = callback;
}

void DragDrop::set_drag_ended_callback(DragEndedCallback callback) {
    drag_ended_callback_ = callback;
}

} // namespace waylaunch
