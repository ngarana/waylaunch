#pragma once

#include "waylaunch/dropdown/placement_backend.h"

#include <map>
#include <string>

namespace waylaunch {

// Phase 4 persisted geometry overrides (docs/DROPDOWN_IMPLEMENTATION.md §5):
// per-slot `w/h` at `$XDG_STATE_HOME/waylaunch/dropdown.tsv`, written via an
// atomic tmp+rename like history.cpp. The resize affordance that *writes*
// overrides is still to come; this store is the load path plus the tested
// round-trip underneath it.
struct DropdownSlotState {
    int w = 0;
    int h = 0;
};

class DropdownStateStore {
  public:
    explicit DropdownStateStore(std::string path = default_path()) : path_(std::move(path)) {}

    static std::string default_path();

    // Slot → override. Malformed lines are skipped; a missing file is empty.
    std::map<std::string, DropdownSlotState> load() const;
    // Read-modify-write of one slot's override, atomically persisted.
    bool save_slot(const std::string& slot, const DropdownSlotState& state);

  private:
    bool save_all(const std::map<std::string, DropdownSlotState>& states) const;
    std::string path_;
};

} // namespace waylaunch
