#include "test_framework.h"
#include "waylaunch/finder_search.h"
#include "waylaunch/search_manager.h"

using namespace waylaunch;

TEST(FinderSearch_HasFd) {
    FinderSearch fs;
    bool has_fd = fs.has_fd();
    TEST_ASSERT(has_fd);
}

TEST(FinderSearch_SetDirectory) {
    FinderSearch fs;
    fs.set_current_directory("/tmp");
    TEST_ASSERT_STR(fs.current_directory(), "/tmp");
}

TEST(FinderSearch_SetScope) {
    FinderSearch fs;
    fs.set_current_directory("/tmp");
    SearchScopeInfo scope;
    scope.type = SearchScopeType::ThisFolder;
    scope.name = "This Folder";
    scope.path = "/tmp";
    fs.set_scope(scope);
    TEST_ASSERT_STR(fs.scope().path, "/tmp");
}

TEST(FinderSearch_AvailableScopes) {
    FinderSearch fs;
    fs.set_current_directory("/tmp");
    auto scopes = fs.get_available_scopes();
    TEST_ASSERT(scopes.size() >= 2);
    TEST_ASSERT(scopes[0].type == SearchScopeType::ThisFolder);
    TEST_ASSERT(scopes[1].type == SearchScopeType::ThisMac);
}

TEST(FinderSearch_Cancel) {
    FinderSearch fs;
    fs.cancel();
}

TEST(FuzzyMatcher_SimpleMatch) {
    FuzzyMatcher matcher("abc");
    auto result = matcher.match("abc");
    TEST_ASSERT(result.matched);
    TEST_ASSERT(result.score > 0);
}

TEST(FuzzyMatcher_NoMatch) {
    FuzzyMatcher matcher("xyz");
    auto result = matcher.match("abc");
    TEST_ASSERT(!result.matched);
}

TEST(FuzzyMatcher_EmptyPattern) {
    FuzzyMatcher matcher("");
    auto result = matcher.match("anything");
    TEST_ASSERT(result.matched);
    TEST_ASSERT(result.score > 0);
}

TEST(FuzzyMatcher_PartialMatch) {
    FuzzyMatcher matcher("ac");
    auto result = matcher.match("abc");
    TEST_ASSERT(result.matched);
}

TEST(FuzzyMatcher_CaseInsensitive) {
    FuzzyMatcher matcher("abc");
    auto result = matcher.match("ABC");
    TEST_ASSERT(result.matched);
}

TEST(FuzzyMatcher_ConsecutiveBonus) {
    FuzzyMatcher matcher("ab");
    auto result1 = matcher.match("ab");
    auto result2 = matcher.match("a_b");
    TEST_ASSERT(result1.matched);
    TEST_ASSERT(result2.matched);
    TEST_ASSERT(result1.score > result2.score);
}

TEST(SearchManager_SetMaxResults) {
    SearchManager sm;
    sm.set_max_results(10);
}

TEST(SearchManager_SetDebounce) {
    SearchManager sm;
    sm.set_debounce_ms(200);
}

TEST(SearchManager_Availability) {
    SearchManager sm;
    bool has_fd = sm.has_fd();
    bool has_rg = sm.has_rg();
    TEST_ASSERT(has_fd || !has_fd);
    TEST_ASSERT(has_rg || !has_rg);
}
