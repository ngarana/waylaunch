#include "test_framework.h"
#include "waylaunch/file_model.h"
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <cstring>

using namespace waylaunch;

TEST(FileEntry_DisplayName) {
    FileEntry fe;
    fe.name = "test.txt";
    TEST_ASSERT_STR(fe.display_name(), "test.txt");
}

TEST(FileEntry_DisplaySize_Bytes) {
    FileEntry fe;
    fe.size = 512;
    TEST_ASSERT_STR(fe.display_size(), "512 B");
}

TEST(FileEntry_DisplaySize_KB) {
    FileEntry fe;
    fe.size = 2048;
    TEST_ASSERT_STR(fe.display_size(), "2.0 KB");
}

TEST(FileEntry_DisplaySize_MB) {
    FileEntry fe;
    fe.size = 5 * 1024 * 1024;
    TEST_ASSERT_STR(fe.display_size(), "5.0 MB");
}

TEST(FileEntry_DisplaySize_GB) {
    FileEntry fe;
    fe.size = 3LL * 1024 * 1024 * 1024;
    TEST_ASSERT_STR(fe.display_size(), "3.0 GB");
}

TEST(FileEntry_DisplaySize_Directory) {
    FileEntry fe;
    fe.is_directory = true;
    fe.size = 4096;
    TEST_ASSERT_STR(fe.display_size(), "--");
}

TEST(FileEntry_DisplayPermissions) {
    FileEntry fe;
    fe.permissions = 0755;
    TEST_ASSERT_STR(fe.display_permissions(), "rwxr-xr-x");
}

TEST(FileEntry_DisplayPermissions_Readonly) {
    FileEntry fe;
    fe.permissions = 0444;
    TEST_ASSERT_STR(fe.display_permissions(), "r--r--r--");
}

TEST(FileEntry_FileKind_Directory) {
    std::string kind = FileModel::file_kind_from_path("/tmp");
    TEST_ASSERT_STR(kind, "Folder");
}

TEST(FileEntry_FileKind_Extensions) {
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".txt"), "Text Document");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".pdf"), "PDF Document");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".cpp"), "C++ Source");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".py"), "Python Script");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".jpg"), "JPEG Image");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".mp4"), "MP4 Video");
    TEST_ASSERT_STR(FileModel::file_kind_from_extension(".zip"), "ZIP Archive");
}

TEST(FileEntry_FileKind_Unknown) {
    TEST_ASSERT(FileModel::file_kind_from_extension(".xyz").find("xyz") != std::string::npos);
}

TEST(FileModel_HomeDirectory) {
    std::string home = FileModel::home_directory();
    TEST_ASSERT(!home.empty());
    TEST_ASSERT(home[0] == '/');
}

TEST(FileModel_NavigateTo) {
    FileModel model;
    model.set_directory("/tmp");
    TEST_ASSERT_STR(model.current_directory(), "/tmp");

    model.navigate_to("/usr");
    TEST_ASSERT_STR(model.current_directory(), "/usr");
}

TEST(FileModel_GoBack) {
    FileModel model;
    model.set_directory("/tmp");
    model.navigate_to("/usr");
    model.go_back();
    TEST_ASSERT_STR(model.current_directory(), "/tmp");
}

TEST(FileModel_GoForward) {
    FileModel model;
    model.set_directory("/tmp");
    model.navigate_to("/usr");
    model.go_back();
    model.go_forward();
    TEST_ASSERT_STR(model.current_directory(), "/usr");
}

TEST(FileModel_GoUp) {
    FileModel model;
    model.set_directory("/usr/local");
    model.go_up();
    TEST_ASSERT_STR(model.current_directory(), "/usr");
}

TEST(FileModel_GoHome) {
    FileModel model;
    model.set_directory("/tmp");
    model.go_home();
    TEST_ASSERT_STR(model.current_directory(), FileModel::home_directory());
}

TEST(FileModel_Entries) {
    FileModel model;
    model.set_directory("/tmp");
    const auto& entries = model.entries();
    TEST_ASSERT(!entries.empty());
    TEST_ASSERT(entries[0].is_directory || !entries[0].name.empty());
}

TEST(FileModel_SortByName) {
    FileModel model;
    model.set_directory("/usr/bin");
    model.set_sort_field(SortField::Name);
    model.set_sort_order(SortOrder::Ascending);

    const auto& sorted = model.sorted_entries();
    TEST_ASSERT(sorted.size() > 1);
    for (size_t i = 1; i < sorted.size(); i++) {
        TEST_ASSERT(strcasecmp(sorted[i-1].name.c_str(), sorted[i].name.c_str()) <= 0);
    }
}

TEST(FileModel_SortBySize) {
    FileModel model;
    model.set_directory("/usr/bin");
    model.set_sort_field(SortField::Size);
    model.set_sort_order(SortOrder::Descending);

    const auto& sorted = model.sorted_entries();
    for (size_t i = 1; i < sorted.size(); i++) {
        TEST_ASSERT(sorted[i-1].size >= sorted[i].size);
    }
}

TEST(FileModel_SelectEntry) {
    FileModel model;
    model.set_directory("/tmp");
    model.select_entry(0);
    TEST_ASSERT_EQ(model.selected_index(), 0);
    TEST_ASSERT(model.is_selected(0));
}

TEST(FileModel_SelectAll) {
    FileModel model;
    model.set_directory("/tmp");
    model.select_all();
    TEST_ASSERT(model.selected_indices().size() == model.sorted_entries().size());
}

TEST(FileModel_DeselectAll) {
    FileModel model;
    model.set_directory("/tmp");
    model.select_all();
    model.deselect_all();
    TEST_ASSERT(model.selected_indices().empty());
}

TEST(FileModel_NavigationState) {
    FileModel model;
    model.set_directory("/tmp");
    model.navigate_to("/usr");

    const auto& nav = model.navigation_state();
    TEST_ASSERT(nav.can_go_back);
    TEST_ASSERT(!nav.can_go_forward);
    TEST_ASSERT(nav.can_go_up);
}

TEST(FileModel_ShowHidden) {
    FileModel model;
    model.set_directory("/tmp");
    model.set_show_hidden(false);
    TEST_ASSERT(!model.show_hidden());

    model.set_show_hidden(true);
    TEST_ASSERT(model.show_hidden());
}

TEST(FileModel_Refresh) {
    FileModel model;
    model.set_directory("/tmp");
    size_t count = model.entries().size();
    model.refresh();
    TEST_ASSERT(model.entries().size() == count);
}

TEST(FileModel_DefaultFavorites) {
    auto favs = FileModel::default_favorites();
    TEST_ASSERT(!favs.empty());
    TEST_ASSERT(favs[0] == FileModel::home_directory());
}
