#pragma once

#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <sstream>
#include <cmath>

namespace test {

struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner runner;
        return runner;
    }

    void add_test(const std::string& name, std::function<void()> test_fn) {
        tests_.push_back({name, test_fn});
    }

    int run_all() {
        int passed = 0;
        int failed = 0;

        std::cout << "\n=== Running " << tests_.size() << " tests ===\n\n";

        for (const auto& test : tests_) {
            try {
                current_test_ = test.name;
                test.fn();
                std::cout << "  PASS: " << test.name << "\n";
                passed++;
            } catch (const std::exception& e) {
                std::cout << "  FAIL: " << test.name << " - " << e.what() << "\n";
                failed++;
            } catch (...) {
                std::cout << "  FAIL: " << test.name << " - Unknown exception\n";
                failed++;
            }
        }

        std::cout << "\n=== Results: " << passed << " passed, " << failed << " failed ===\n\n";
        return failed;
    }

    std::string current_test_name() const { return current_test_; }

private:
    struct Test {
        std::string name;
        std::function<void()> fn;
    };
    std::vector<Test> tests_;
    std::string current_test_;
};

class TestRegistrar {
public:
    TestRegistrar(const std::string& name, std::function<void()> fn) {
        TestRunner::instance().add_test(name, fn);
    }
};

} // namespace test

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #cond " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b) \
    do { \
        auto va = (a); auto vb = (b); \
        if (va != vb) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " == " #b " (" << va << " != " << vb << ") at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST_ASSERT_STR(a, b) \
    do { \
        if ((a) != (b)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " == " #b " (\"" << (a) << "\" != \"" << (b) << "\") at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST_ASSERT_NEAR(a, b, eps) \
    do { \
        if (std::abs((a) - (b)) > (eps)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: |" #a " - " #b "| <= " #eps " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST(name) \
    static void test_fn_##name(); \
    static test::TestRegistrar reg_##name(#name, test_fn_##name); \
    static void test_fn_##name()
