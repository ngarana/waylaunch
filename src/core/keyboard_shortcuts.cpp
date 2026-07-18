#include "waylaunch/keyboard_shortcuts.h"
#include <algorithm>

namespace waylaunch {

KeyboardShortcuts::KeyboardShortcuts() = default;
KeyboardShortcuts::~KeyboardShortcuts() = default;

void KeyboardShortcuts::load_defaults() {
    clear();
    add_binding(0xff51, true, false, false, false, ShortcutAction::Back);
    add_binding(0xff53, true, false, false, false, ShortcutAction::Forward);
    add_binding(0xff52, true, false, false, false, ShortcutAction::Up);
    add_binding(0xff50, false, false, false, false, ShortcutAction::Home);
    add_binding(0x31, true, false, false, false, ShortcutAction::ViewList);
    add_binding(0x32, true, false, false, false, ShortcutAction::ViewIcon);
    add_binding(0x33, true, false, false, false, ShortcutAction::ViewColumn);
    add_binding(0x34, true, false, false, false, ShortcutAction::ViewGallery);
    add_binding(0x74, true, false, false, false, ShortcutAction::NewTab);
    add_binding(0x77, true, false, false, false, ShortcutAction::CloseTab);
    add_binding(0xff09, true, false, false, false, ShortcutAction::NextTab);
    add_binding(0xff09, true, true, false, false, ShortcutAction::PreviousTab);
    add_binding(0x6e, true, false, false, false, ShortcutAction::NewFolder);
    add_binding(0x64, true, false, false, false, ShortcutAction::Duplicate);
    add_binding(0xff08, true, false, false, false, ShortcutAction::Delete);
    add_binding(0x61, true, false, false, false, ShortcutAction::SelectAll);
    add_binding(0x20, false, false, false, false, ShortcutAction::Preview);
    add_binding(0x69, true, false, false, false, ShortcutAction::Info);
    add_binding(0x66, true, false, false, false, ShortcutAction::Search);
    add_binding(0x68, true, false, false, false, ShortcutAction::ToggleHidden);
    add_binding(0x63, true, false, false, false, ShortcutAction::Copy);
    add_binding(0x78, true, false, false, false, ShortcutAction::Cut);
    add_binding(0x76, true, false, false, false, ShortcutAction::Paste);
    add_binding(0xff1b, false, false, false, false, ShortcutAction::CloseTab);
}

void KeyboardShortcuts::add_binding(uint32_t keysym, bool ctrl, bool shift, bool alt, bool super, ShortcutAction action) {
    bindings_.push_back({keysym, ctrl, shift, alt, super, action});
}

void KeyboardShortcuts::remove_binding(ShortcutAction action) {
    bindings_.erase(
        std::remove_if(bindings_.begin(), bindings_.end(),
            [&](const KeyBinding& b) { return b.action == action; }),
        bindings_.end());
}

void KeyboardShortcuts::clear() { bindings_.clear(); }

ShortcutAction KeyboardShortcuts::find_action(uint32_t keysym, bool ctrl, bool shift, bool alt, bool super) const {
    for (const auto& b : bindings_) {
        if (b.keysym == keysym && b.ctrl == ctrl && b.shift == shift && b.alt == alt && b.super == super)
            return b.action;
    }
    return ShortcutAction::None;
}

KeyBinding KeyboardShortcuts::find_binding(ShortcutAction action) const {
    for (const auto& b : bindings_) {
        if (b.action == action) return b;
    }
    return KeyBinding{};
}

std::vector<KeyBinding> KeyboardShortcuts::get_all_bindings() const { return bindings_; }

std::string KeyboardShortcuts::action_name(ShortcutAction action) {
    switch (action) {
        case ShortcutAction::Back: return "Back";
        case ShortcutAction::Forward: return "Forward";
        case ShortcutAction::Up: return "Up";
        case ShortcutAction::Home: return "Home";
        case ShortcutAction::ViewList: return "ViewList";
        case ShortcutAction::ViewIcon: return "ViewIcon";
        case ShortcutAction::ViewColumn: return "ViewColumn";
        case ShortcutAction::ViewGallery: return "ViewGallery";
        case ShortcutAction::NewTab: return "NewTab";
        case ShortcutAction::CloseTab: return "CloseTab";
        case ShortcutAction::NextTab: return "NextTab";
        case ShortcutAction::PreviousTab: return "PreviousTab";
        case ShortcutAction::NewFolder: return "NewFolder";
        case ShortcutAction::Duplicate: return "Duplicate";
        case ShortcutAction::Delete: return "Delete";
        case ShortcutAction::Rename: return "Rename";
        case ShortcutAction::SelectAll: return "SelectAll";
        case ShortcutAction::DeselectAll: return "DeselectAll";
        case ShortcutAction::Preview: return "Preview";
        case ShortcutAction::Info: return "Info";
        case ShortcutAction::Search: return "Search";
        case ShortcutAction::ToggleHidden: return "ToggleHidden";
        case ShortcutAction::Copy: return "Copy";
        case ShortcutAction::Cut: return "Cut";
        case ShortcutAction::Paste: return "Paste";
        default: return "None";
    }
}

std::string KeyboardShortcuts::action_description(ShortcutAction action) {
    switch (action) {
        case ShortcutAction::Back: return "Go back";
        case ShortcutAction::Forward: return "Go forward";
        case ShortcutAction::Up: return "Go up one level";
        case ShortcutAction::Home: return "Go to home folder";
        case ShortcutAction::ViewList: return "Switch to list view";
        case ShortcutAction::ViewIcon: return "Switch to icon view";
        case ShortcutAction::ViewColumn: return "Switch to column view";
        case ShortcutAction::ViewGallery: return "Switch to gallery view";
        case ShortcutAction::NewTab: return "Open new tab";
        case ShortcutAction::CloseTab: return "Close current tab";
        case ShortcutAction::NextTab: return "Switch to next tab";
        case ShortcutAction::PreviousTab: return "Switch to previous tab";
        case ShortcutAction::NewFolder: return "Create new folder";
        case ShortcutAction::Duplicate: return "Duplicate selected item";
        case ShortcutAction::Delete: return "Move to trash";
        case ShortcutAction::Rename: return "Rename selected item";
        case ShortcutAction::SelectAll: return "Select all items";
        case ShortcutAction::DeselectAll: return "Deselect all items";
        case ShortcutAction::Preview: return "Toggle Quick Look preview";
        case ShortcutAction::Info: return "Show Get Info panel";
        case ShortcutAction::Search: return "Focus search bar";
        case ShortcutAction::ToggleHidden: return "Toggle hidden files";
        case ShortcutAction::Copy: return "Copy selected items";
        case ShortcutAction::Cut: return "Cut selected items";
        case ShortcutAction::Paste: return "Paste items";
        default: return "";
    }
}

std::string KeyboardShortcuts::format_shortcut(const KeyBinding& binding) {
    std::string r;
    if (binding.ctrl) r += "Ctrl+";
    if (binding.shift) r += "Shift+";
    if (binding.alt) r += "Alt+";
    if (binding.super) r += "Super+";
    if (binding.keysym >= 0xff00) {
        switch (binding.keysym) {
            case 0xff08: r += "Backspace"; break;
            case 0xff09: r += "Tab"; break;
            case 0xff0d: r += "Return"; break;
            case 0xff1b: r += "Escape"; break;
            case 0xff50: r += "Home"; break;
            case 0xff51: r += "Left"; break;
            case 0xff52: r += "Up"; break;
            case 0xff53: r += "Right"; break;
            case 0xff54: r += "Down"; break;
            case 0xff55: r += "PageUp"; break;
            case 0xff56: r += "PageDown"; break;
            case 0xff57: r += "End"; break;
            case 0xff0b: r += "Insert"; break;
            case 0xff20: r += "Delete"; break;
            default: r += "Key"; break;
        }
    } else {
        r += static_cast<char>(binding.keysym);
    }
    return r;
}

} // namespace waylaunch
