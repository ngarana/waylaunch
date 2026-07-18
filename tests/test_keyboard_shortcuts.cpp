#include "test_framework.h"
#include "waylaunch/keyboard_shortcuts.h"

using namespace waylaunch;

TEST(KeyboardShortcuts_LoadDefaults) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(!ks.get_all_bindings().empty());
}

TEST(KeyboardShortcuts_FindAction) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0x31, true, false, false, false) == ShortcutAction::ViewList);
    TEST_ASSERT(ks.find_action(0x32, true, false, false, false) == ShortcutAction::ViewIcon);
    TEST_ASSERT(ks.find_action(0x33, true, false, false, false) == ShortcutAction::ViewColumn);
    TEST_ASSERT(ks.find_action(0x34, true, false, false, false) == ShortcutAction::ViewGallery);
}

TEST(KeyboardShortcuts_FindAction_None) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0xff00, false, false, false, false) == ShortcutAction::None);
}

TEST(KeyboardShortcuts_FindAction_Modifiers) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0x74, true, false, false, false) == ShortcutAction::NewTab);
    TEST_ASSERT(ks.find_action(0x77, true, false, false, false) == ShortcutAction::CloseTab);
    TEST_ASSERT(ks.find_action(0x76, true, false, false, false) == ShortcutAction::Paste);
}

TEST(KeyboardShortcuts_FindBinding) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    auto binding = ks.find_binding(ShortcutAction::NewTab);
    TEST_ASSERT(binding.action == ShortcutAction::NewTab);
    TEST_ASSERT(binding.ctrl);
    TEST_ASSERT(binding.keysym == 0x74);
}

TEST(KeyboardShortcuts_FindBinding_None) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    auto binding = ks.find_binding(ShortcutAction::None);
    TEST_ASSERT(binding.action == ShortcutAction::None);
}

TEST(KeyboardShortcuts_AddBinding) {
    KeyboardShortcuts ks;
    size_t before = ks.get_all_bindings().size();
    ks.add_binding(0xffbe, false, false, false, false, ShortcutAction::Preview);
    TEST_ASSERT_EQ(ks.get_all_bindings().size(), before + 1);
    TEST_ASSERT(ks.find_action(0xffbe, false, false, false, false) == ShortcutAction::Preview);
}

TEST(KeyboardShortcuts_RemoveBinding) {
    KeyboardShortcuts ks;
    ks.add_binding(0x42, false, false, false, false, ShortcutAction::Info);
    ks.remove_binding(ShortcutAction::Info);
    TEST_ASSERT(ks.find_action(0x42, false, false, false, false) == ShortcutAction::None);
}

TEST(KeyboardShortcuts_Clear) {
    KeyboardShortcuts ks;
    ks.add_binding(0x42, false, false, false, false, ShortcutAction::Info);
    ks.clear();
    TEST_ASSERT(ks.get_all_bindings().empty());
}

TEST(KeyboardShortcuts_ActionNames) {
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::Back), "Back");
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::Forward), "Forward");
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::Copy), "Copy");
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::Paste), "Paste");
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::Delete), "Delete");
    TEST_ASSERT_STR(KeyboardShortcuts::action_name(ShortcutAction::None), "None");
}

TEST(KeyboardShortcuts_ActionDescriptions) {
    TEST_ASSERT(!KeyboardShortcuts::action_description(ShortcutAction::Back).empty());
    TEST_ASSERT(!KeyboardShortcuts::action_description(ShortcutAction::NewTab).empty());
    TEST_ASSERT(!KeyboardShortcuts::action_description(ShortcutAction::SelectAll).empty());
    TEST_ASSERT(KeyboardShortcuts::action_description(ShortcutAction::None).empty());
}

TEST(KeyboardShortcuts_FormatShortcut) {
    KeyboardShortcuts ks;
    KeyBinding b;
    b.keysym = 0x74;
    b.ctrl = true;
    std::string formatted = KeyboardShortcuts::format_shortcut(b);
    TEST_ASSERT(formatted.find("Ctrl") != std::string::npos);
    TEST_ASSERT(formatted.find("t") != std::string::npos);
}

TEST(KeyboardShortcuts_FormatShortcut_Escape) {
    KeyboardShortcuts ks;
    KeyBinding b;
    b.keysym = 0xff1b;
    std::string formatted = KeyboardShortcuts::format_shortcut(b);
    TEST_ASSERT(formatted.find("Escape") != std::string::npos);
}

TEST(KeyboardShortcuts_DefaultNavigation) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0xff51, true, false, false, false) == ShortcutAction::Back);
    TEST_ASSERT(ks.find_action(0xff53, true, false, false, false) == ShortcutAction::Forward);
    TEST_ASSERT(ks.find_action(0xff52, true, false, false, false) == ShortcutAction::Up);
}

TEST(KeyboardShortcuts_DefaultFileOps) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0x6e, true, false, false, false) == ShortcutAction::NewFolder);
    TEST_ASSERT(ks.find_action(0x64, true, false, false, false) == ShortcutAction::Duplicate);
    TEST_ASSERT(ks.find_action(0xff08, true, false, false, false) == ShortcutAction::Delete);
}

TEST(KeyboardShortcuts_DefaultViewToggle) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0x31, true, false, false, false) == ShortcutAction::ViewList);
    TEST_ASSERT(ks.find_action(0x32, true, false, false, false) == ShortcutAction::ViewIcon);
    TEST_ASSERT(ks.find_action(0x33, true, false, false, false) == ShortcutAction::ViewColumn);
    TEST_ASSERT(ks.find_action(0x34, true, false, false, false) == ShortcutAction::ViewGallery);
}

TEST(KeyboardShortcuts_DefaultClipboard) {
    KeyboardShortcuts ks;
    ks.load_defaults();
    TEST_ASSERT(ks.find_action(0x63, true, false, false, false) == ShortcutAction::Copy);
    TEST_ASSERT(ks.find_action(0x78, true, false, false, false) == ShortcutAction::Cut);
    TEST_ASSERT(ks.find_action(0x76, true, false, false, false) == ShortcutAction::Paste);
}
