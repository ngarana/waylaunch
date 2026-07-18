#include "test_framework.h"
#include "waylaunch/file_ops.h"
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>

using namespace waylaunch;

static const std::string TEST_DIR = "/tmp/waylaunch_test";
static const std::string TEST_FILE = TEST_DIR + "/test_file.txt";
static const std::string TEST_CONTENT = "Hello, World!";

static void setup_test_env() {
    std::filesystem::create_directories(TEST_DIR);
    std::ofstream file(TEST_FILE);
    file << TEST_CONTENT;
    file.close();
}

static void cleanup_test_env() {
    std::filesystem::remove_all(TEST_DIR);
}

TEST(FileOps_Copy) {
    setup_test_env();
    FileOperations ops;
    std::string dest = TEST_DIR + "/copied.txt";
    auto result = ops.copy(TEST_FILE, dest);
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(dest));
    cleanup_test_env();
}

TEST(FileOps_Copy_NonExistent) {
    FileOperations ops;
    auto result = ops.copy("/nonexistent/file.txt", "/tmp/nope.txt");
    TEST_ASSERT(!result.success);
    TEST_ASSERT(!result.error_message.empty());
}

TEST(FileOps_Move) {
    setup_test_env();
    FileOperations ops;
    std::string dest = TEST_DIR + "/moved.txt";
    auto result = ops.move(TEST_FILE, dest);
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(dest));
    TEST_ASSERT(!std::filesystem::exists(TEST_FILE));
    cleanup_test_env();
}

TEST(FileOps_Rename) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.rename(TEST_FILE, "renamed.txt");
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(TEST_DIR + "/renamed.txt"));
    cleanup_test_env();
}

TEST(FileOps_Rename_Conflict) {
    setup_test_env();
    FileOperations ops;
    std::ofstream(TEST_DIR + "/existing.txt") << "data";
    auto result = ops.rename(TEST_FILE, "existing.txt");
    TEST_ASSERT(!result.success);
    cleanup_test_env();
}

TEST(FileOps_Remove_ToTrash) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.remove(TEST_FILE);
    TEST_ASSERT(result.success);
    TEST_ASSERT(!std::filesystem::exists(TEST_FILE));
    cleanup_test_env();
}

TEST(FileOps_Permanent_Delete) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.permanent_delete(TEST_FILE);
    TEST_ASSERT(result.success);
    TEST_ASSERT(!std::filesystem::exists(TEST_FILE));
    cleanup_test_env();
}

TEST(FileOps_Duplicate) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.duplicate(TEST_FILE);
    TEST_ASSERT(result.success);
    TEST_ASSERT(!result.destination.empty());
    TEST_ASSERT(std::filesystem::exists(result.destination));
    cleanup_test_env();
}

TEST(FileOps_NewFolder) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.new_folder(TEST_DIR, "new_folder");
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(TEST_DIR + "/new_folder"));
    cleanup_test_env();
}

TEST(FileOps_NewFolder_DefaultName) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.new_folder(TEST_DIR, "");
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(TEST_DIR + "/New Folder"));
    cleanup_test_env();
}

TEST(FileOps_NewFolder_Duplicate) {
    setup_test_env();
    FileOperations ops;
    ops.new_folder(TEST_DIR, "folder");
    auto result = ops.new_folder(TEST_DIR, "folder");
    TEST_ASSERT(result.success);
    TEST_ASSERT(std::filesystem::exists(TEST_DIR + "/folder 1"));
    cleanup_test_env();
}

TEST(FileOps_MoveToTrash) {
    setup_test_env();
    FileOperations ops;
    auto result = ops.move_to_trash(TEST_FILE);
    TEST_ASSERT(result.success);
    TEST_ASSERT(!std::filesystem::exists(TEST_FILE));
    cleanup_test_env();
}

TEST(FileOps_EmptyTrash) {
    FileOperations ops;
    auto result = ops.empty_trash();
    TEST_ASSERT(result.success);
}

TEST(FileOps_FileExists) {
    setup_test_env();
    FileOperations ops;
    TEST_ASSERT(ops.file_exists(TEST_FILE));
    TEST_ASSERT(!ops.file_exists("/nonexistent/file.txt"));
    cleanup_test_env();
}

TEST(FileOps_IsDirectory) {
    setup_test_env();
    FileOperations ops;
    TEST_ASSERT(ops.is_directory(TEST_DIR));
    TEST_ASSERT(!ops.is_directory(TEST_FILE));
    cleanup_test_env();
}

TEST(FileOps_GenerateUniqueName) {
    setup_test_env();
    FileOperations ops;
    std::ofstream(TEST_DIR + "/test.txt") << "";
    std::string name = ops.generate_unique_name(TEST_DIR, "test.txt");
    TEST_ASSERT(name != "test.txt");
    TEST_ASSERT(name.find("test") != std::string::npos);
    cleanup_test_env();
}

TEST(FileOps_CreateDirectory) {
    setup_test_env();
    FileOperations ops;
    bool ok = ops.create_directory(TEST_DIR + "/nested/deep/dir");
    TEST_ASSERT(ok);
    TEST_ASSERT(std::filesystem::exists(TEST_DIR + "/nested/deep/dir"));
    cleanup_test_env();
}
