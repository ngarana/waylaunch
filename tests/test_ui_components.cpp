#include "test_framework.h"
#include "waylaunch/tabs.h"
#include "waylaunch/sidebar.h"
#include "waylaunch/toolbar.h"
#include "waylaunch/status_bar.h"
#include "waylaunch/preview.h"
#include "waylaunch/info_panel.h"
#include "waylaunch/sort_menu.h"
#include "waylaunch/batch_rename.h"
#include "waylaunch/drag_drop.h"

using namespace waylaunch;

TEST(Tabs_AddTab) {
    Tabs tabs;
    std::string id = tabs.add_tab("/tmp");
    TEST_ASSERT(!id.empty());
    TEST_ASSERT_EQ(tabs.tab_count(), 1);
    TEST_ASSERT(tabs.active_tab() != nullptr);
}

TEST(Tabs_AddMultipleTabs) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.add_tab("/home");
    TEST_ASSERT_EQ(tabs.tab_count(), 3);
    TEST_ASSERT_STR(tabs.active_tab_path(), "/tmp");
}

TEST(Tabs_CloseTab) {
    Tabs tabs;
    std::string id1 = tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.close_tab(id1);
    TEST_ASSERT_EQ(tabs.tab_count(), 1);
}

TEST(Tabs_CloseTab_ByIndex) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.close_tab(0);
    TEST_ASSERT_EQ(tabs.tab_count(), 1);
    TEST_ASSERT_STR(tabs.active_tab_path(), "/usr");
}

TEST(Tabs_CloseOtherTabs) {
    Tabs tabs;
    std::string id1 = tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.add_tab("/home");
    tabs.close_other_tabs(id1);
    TEST_ASSERT_EQ(tabs.tab_count(), 1);
}

TEST(Tabs_ActivateTab) {
    Tabs tabs;
    std::string id1 = tabs.add_tab("/tmp");
    std::string id2 = tabs.add_tab("/usr");
    tabs.activate_tab(id1);
    TEST_ASSERT_STR(tabs.active_tab_path(), "/tmp");
}

TEST(Tabs_ActivateNext) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.activate_next_tab();
    TEST_ASSERT_STR(tabs.active_tab_path(), "/usr");
}

TEST(Tabs_ActivatePrevious) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.activate_next_tab();
    tabs.activate_previous_tab();
    TEST_ASSERT_STR(tabs.active_tab_path(), "/tmp");
}

TEST(Tabs_WrapAround) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    tabs.add_tab("/usr");
    tabs.activate_next_tab();
    tabs.activate_next_tab();
    TEST_ASSERT_STR(tabs.active_tab_path(), "/tmp");
}

TEST(Tabs_SetTabPath) {
    Tabs tabs;
    std::string id = tabs.add_tab("/tmp");
    tabs.set_tab_path(id, "/usr");
    TEST_ASSERT_STR(tabs.active_tab_path(), "/usr");
}

TEST(Tabs_HitTest) {
    Tabs tabs;
    tabs.add_tab("/tmp");
    int idx = tabs.hit_test(50, 16);
    TEST_ASSERT(idx >= 0);
}

TEST(Tabs_EmptyHitTest) {
    Tabs tabs;
    int idx = tabs.hit_test(50, 16);
    TEST_ASSERT(idx < 0);
}

TEST(Sidebar_LoadFavorites) {
    Sidebar sb;
    sb.load_favorites();
    TEST_ASSERT(sb.get_item(0) != nullptr);
}

TEST(Sidebar_SelectedPath) {
    Sidebar sb;
    sb.set_selected_path("/tmp");
    TEST_ASSERT_STR(sb.selected_path(), "/tmp");
}

TEST(Sidebar_Width) {
    Sidebar sb;
    sb.set_width(250);
    TEST_ASSERT_EQ(sb.width(), 250);
}

TEST(Toolbar_SetPath) {
    Toolbar tb;
    tb.set_current_path("/usr/local/bin");
    TEST_ASSERT_STR(tb.current_path(), "/usr/local/bin");
}

TEST(Toolbar_Height) {
    Toolbar tb;
    tb.set_height(48);
    TEST_ASSERT_EQ(tb.height(), 48);
}

TEST(Toolbar_BackForward) {
    Toolbar tb;
    tb.set_back_enabled(true);
    tb.set_forward_enabled(true);
}

TEST(StatusBar_SetCounts) {
    StatusBar sb;
    sb.set_item_count(42, 3);
}

TEST(StatusBar_SetSpace) {
    StatusBar sb;
    sb.set_available_space(1024 * 1024 * 1024LL);
}

TEST(Preview_SetWidth) {
    Preview p;
    p.set_width(350);
    TEST_ASSERT_EQ(p.width(), 350);
}

TEST(Preview_Visibility) {
    Preview p;
    TEST_ASSERT(!p.is_visible());
    p.set_visible(true);
    TEST_ASSERT(p.is_visible());
}

TEST(Preview_LoadFile_Text) {
    Preview p;
    p.load_file("/etc/hostname");
    TEST_ASSERT(p.content().type == PreviewType::Text || p.content().type == PreviewType::Generic);
    TEST_ASSERT(!p.file_entry().name.empty());
}

TEST(Preview_LoadFile_Nonexistent) {
    Preview p;
    p.load_file("/nonexistent/file.txt");
    TEST_ASSERT(!p.content().error.empty());
}

TEST(Preview_Clear) {
    Preview p;
    p.load_file("/etc/hostname");
    p.clear();
    TEST_ASSERT(p.content().file_path.empty());
}

TEST(InfoPanel_Width) {
    InfoPanel ip;
    ip.set_width(320);
    TEST_ASSERT_EQ(ip.width(), 320);
}

TEST(InfoPanel_Visibility) {
    InfoPanel ip;
    TEST_ASSERT(!ip.is_visible());
    ip.set_visible(true);
    TEST_ASSERT(ip.is_visible());
}

TEST(InfoPanel_LoadFile) {
    InfoPanel ip;
    ip.load_file("/etc/hostname");
    TEST_ASSERT(!ip.data().name.empty());
    TEST_ASSERT(!ip.data().kind.empty());
}

TEST(InfoPanel_LoadDirectory) {
    InfoPanel ip;
    ip.load_file("/tmp");
    TEST_ASSERT(ip.data().is_directory);
}

TEST(InfoPanel_Clear) {
    InfoPanel ip;
    ip.load_file("/etc/hostname");
    ip.clear();
    TEST_ASSERT(ip.data().name.empty());
}

TEST(SortMenu_Visibility) {
    SortMenu sm;
    TEST_ASSERT(!sm.is_visible());
    sm.set_visible(true);
    TEST_ASSERT(sm.is_visible());
}

TEST(SortMenu_SetSortField) {
    SortMenu sm;
    sm.set_sort_field(SortField::Size);
}

TEST(SortMenu_SetGroupMode) {
    SortMenu sm;
    sm.set_group_mode(GroupMode::Kind);
}

TEST(BatchRename_SetMode) {
    BatchRename br;
    br.set_mode(RenameMode::Lowercase);
    TEST_ASSERT(br.mode() == RenameMode::Lowercase);
}

TEST(BatchRename_SetFiles) {
    BatchRename br;
    br.set_files({"/tmp/a.txt", "/tmp/b.txt", "/tmp/c.txt"});
    TEST_ASSERT_EQ(br.files().size(), 3u);
}

TEST(BatchRename_FindReplace) {
    BatchRename br;
    br.set_files({"/tmp/old_name.txt"});
    br.set_mode(RenameMode::FindReplace);
    br.set_find_text("old");
    br.set_replace_text("new");
    br.update_preview();
    TEST_ASSERT(br.preview().size() == 1);
    TEST_ASSERT(br.preview()[0].new_name.find("new") != std::string::npos);
}

TEST(BatchRename_Prefix) {
    BatchRename br;
    br.set_files({"/tmp/file.txt"});
    br.set_mode(RenameMode::AddPrefix);
    br.set_prefix("prefix_");
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name.find("prefix_") != std::string::npos);
}

TEST(BatchRename_Suffix) {
    BatchRename br;
    br.set_files({"/tmp/file.txt"});
    br.set_mode(RenameMode::AddSuffix);
    br.set_suffix("_suffix");
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name.find("_suffix") != std::string::npos);
}

TEST(BatchRename_Sequential) {
    BatchRename br;
    br.set_files({"/tmp/a.txt", "/tmp/b.txt", "/tmp/c.txt"});
    br.set_mode(RenameMode::Sequential);
    br.set_start_number(1);
    br.set_digits(3);
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name.find("001") != std::string::npos);
    TEST_ASSERT(br.preview()[2].new_name.find("003") != std::string::npos);
}

TEST(BatchRename_Lowercase) {
    BatchRename br;
    br.set_files({"/tmp/MYFILE.txt"});
    br.set_mode(RenameMode::Lowercase);
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name == "myfile.txt");
}

TEST(BatchRename_Uppercase) {
    BatchRename br;
    br.set_files({"/tmp/myfile.txt"});
    br.set_mode(RenameMode::Uppercase);
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name == "MYFILE.txt");
}

TEST(BatchRename_ReplaceSpaces) {
    BatchRename br;
    br.set_files({"/tmp/my file.txt"});
    br.set_mode(RenameMode::ReplaceSpaces);
    br.update_preview();
    TEST_ASSERT(br.preview()[0].new_name == "my_file.txt");
}

TEST(BatchRename_HasErrors_NoErrors) {
    BatchRename br;
    br.set_files({"/tmp/a.txt"});
    br.set_mode(RenameMode::Lowercase);
    br.update_preview();
    TEST_ASSERT(!br.has_errors());
}

TEST(DragDrop_StartDrag) {
    DragDrop dd;
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Move);
    TEST_ASSERT(dd.is_dragging());
    TEST_ASSERT_EQ(dd.drag_data().paths.size(), 1u);
}

TEST(DragDrop_UpdateDrag) {
    DragDrop dd;
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Copy);
    dd.update_drag(150, 250);
    TEST_ASSERT(dd.drag_data().current_x == 150);
    TEST_ASSERT(dd.drag_data().current_y == 250);
    TEST_ASSERT(dd.drag_data().action == DragAction::Copy);
}

TEST(DragDrop_EndDrag) {
    DragDrop dd;
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Move);
    dd.set_drop_target("/tmp/target_dir");
    dd.end_drag();
    TEST_ASSERT(!dd.is_dragging());
}

TEST(DragDrop_CancelDrag) {
    DragDrop dd;
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Move);
    dd.cancel_drag();
    TEST_ASSERT(!dd.is_dragging());
}

TEST(DragDrop_DropTarget) {
    DragDrop dd;
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Move);
    dd.set_drop_target("/usr/local");
    TEST_ASSERT_STR(dd.drop_target(), "/usr/local");
}

TEST(DragDrop_IsDragging) {
    DragDrop dd;
    TEST_ASSERT(!dd.is_dragging());
    dd.start_drag({"/tmp/file.txt"}, 100, 200, DragAction::Move);
    TEST_ASSERT(dd.is_dragging());
    dd.end_drag();
    TEST_ASSERT(!dd.is_dragging());
}
