#include "test_framework.h"
#include "waylaunch/tags.h"
#include <unistd.h>
#include <filesystem>

using namespace waylaunch;

TEST(TagsManager_CreateTag) {
    TagsManager tm;
    tm.load();
    tm.create_tag("test_tag_red", "#ff0000");
    TEST_ASSERT(tm.has_tag("test_tag_red"));
    tm.delete_tag("test_tag_red");
}

TEST(TagsManager_DeleteTag) {
    TagsManager tm;
    tm.load();
    tm.create_tag("test_del", "#00ff00");
    TEST_ASSERT(tm.has_tag("test_del"));
    tm.delete_tag("test_del");
    TEST_ASSERT(!tm.has_tag("test_del"));
}

TEST(TagsManager_RenameTag) {
    TagsManager tm;
    tm.load();
    tm.create_tag("old_name", "#0000ff");
    tm.rename_tag("old_name", "new_name");
    TEST_ASSERT(!tm.has_tag("old_name"));
    TEST_ASSERT(tm.has_tag("new_name"));
    tm.delete_tag("new_name");
}

TEST(TagsManager_SetColor) {
    TagsManager tm;
    tm.load();
    tm.create_tag("color_test", "#ff0000");
    tm.set_tag_color("color_test", "#00ff00");
    auto tag = tm.get_tag("color_test");
    TEST_ASSERT_STR(tag.color, "#00ff00");
    tm.delete_tag("color_test");
}

TEST(TagsManager_TagFile) {
    TagsManager tm;
    tm.load();
    tm.create_tag("file_tag_test", "#ff00ff");
    tm.tag_file("/tmp/test_file_for_tagging.txt", "file_tag_test");
    TEST_ASSERT(tm.is_file_tagged("/tmp/test_file_for_tagging.txt", "file_tag_test"));
    tm.untag_file("/tmp/test_file_for_tagging.txt", "file_tag_test");
    tm.delete_tag("file_tag_test");
}

TEST(TagsManager_UntagFile) {
    TagsManager tm;
    tm.load();
    tm.create_tag("untag_test", "#ffff00");
    tm.tag_file("/tmp/untag_test_file.txt", "untag_test");
    TEST_ASSERT(tm.is_file_tagged("/tmp/untag_test_file.txt", "untag_test"));
    tm.untag_file("/tmp/untag_test_file.txt", "untag_test");
    TEST_ASSERT(!tm.is_file_tagged("/tmp/untag_test_file.txt", "untag_test"));
    tm.delete_tag("untag_test");
}

TEST(TagsManager_GetFileTags) {
    TagsManager tm;
    tm.load();
    tm.create_tag("get_tags_a", "#ff0000");
    tm.create_tag("get_tags_b", "#00ff00");
    tm.tag_file("/tmp/multi_tag.txt", "get_tags_a");
    tm.tag_file("/tmp/multi_tag.txt", "get_tags_b");
    auto tags = tm.get_file_tags("/tmp/multi_tag.txt");
    TEST_ASSERT(tags.size() == 2);
    tm.untag_file("/tmp/multi_tag.txt", "get_tags_a");
    tm.untag_file("/tmp/multi_tag.txt", "get_tags_b");
    tm.delete_tag("get_tags_a");
    tm.delete_tag("get_tags_b");
}

TEST(TagsManager_GetFilesByTag) {
    TagsManager tm;
    tm.load();
    tm.create_tag("by_tag_test", "#ff8800");
    tm.tag_file("/tmp/by_tag_1.txt", "by_tag_test");
    tm.tag_file("/tmp/by_tag_2.txt", "by_tag_test");
    auto files = tm.get_files_by_tag("by_tag_test");
    TEST_ASSERT(files.size() == 2);
    tm.untag_file("/tmp/by_tag_1.txt", "by_tag_test");
    tm.untag_file("/tmp/by_tag_2.txt", "by_tag_test");
    tm.delete_tag("by_tag_test");
}

TEST(TagsManager_SearchTags) {
    TagsManager tm;
    tm.load();
    tm.create_tag("search_alpha", "#ff0000");
    tm.create_tag("search_beta", "#00ff00");
    auto results = tm.search_tags("search");
    TEST_ASSERT(results.size() >= 2);
    tm.delete_tag("search_alpha");
    tm.delete_tag("search_beta");
}

TEST(TagsManager_DefaultColors) {
    auto colors = TagsManager::get_default_tag_colors();
    TEST_ASSERT(colors.size() == 8);
    TEST_ASSERT(colors[0] == "#ff3b30");
}

TEST(TagsManager_TagFile_NonexistentTag) {
    TagsManager tm;
    tm.load();
    bool ok = tm.tag_file("/tmp/file.txt", "nonexistent_tag_xyz");
    TEST_ASSERT(!ok);
}

TEST(TagsManager_GetTag) {
    TagsManager tm;
    tm.load();
    tm.create_tag("get_test", "#aabbcc");
    auto tag = tm.get_tag("get_test");
    TEST_ASSERT_STR(tag.name, "get_test");
    TEST_ASSERT_STR(tag.color, "#aabbcc");
    tm.delete_tag("get_test");
}

TEST(TagsManager_GetTag_Nonexistent) {
    TagsManager tm;
    tm.load();
    auto tag = tm.get_tag("this_tag_does_not_exist_xyz");
    TEST_ASSERT(tag.name.empty());
}

TEST(TagsManager_CreateDuplicate) {
    TagsManager tm;
    tm.load();
    tm.create_tag("dup_test", "#ff0000");
    bool ok = tm.create_tag("dup_test", "#0000ff");
    TEST_ASSERT(!ok);
    tm.delete_tag("dup_test");
}

TEST(TagsManager_RenameToExisting) {
    TagsManager tm;
    tm.load();
    tm.create_tag("rename_a", "#ff0000");
    tm.create_tag("rename_b", "#00ff00");
    bool ok = tm.rename_tag("rename_a", "rename_b");
    TEST_ASSERT(!ok);
    tm.delete_tag("rename_a");
    tm.delete_tag("rename_b");
}
