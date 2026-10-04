/**
 * @file    framework.cpp
 * @brief   Registration table and runner of the Cinux test framework.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 2.0
 * @since   0.1.0
 * @ingroup test_framework
 */

#include "framework.hpp"

#include <print>
#include <source_location>

#include "test_case.hpp"

namespace cinux::test {

namespace {

constexpr int kMaxCases = 256;

TestCase g_table[kMaxCases];
int      g_table_size = 0;

}  // namespace

int g_failures = 0;

namespace {

const char* g_current_case_name = "";

const char* lookup_case_name() {
    return g_current_case_name;
}

}  // namespace

const char* CurrentCaseName() {
    return lookup_case_name();
}

void ReportCheckFailure(const char* expression, std::source_location loc) {
    std::println(stderr, "[FAIL] {}\n  ASSERT_TRUE({}) failed\n  at {}:{}", CurrentCaseName(),
                 expression, loc.file_name(), loc.line());
}

void ReportCompareFailure(const char* actual_text, const char* expected_text, bool expected_equal,
                          std::source_location loc) {
    const char* const kAssertion = expected_equal ? "ASSERT_EQ" : "ASSERT_NE";
    std::println(stderr, "[FAIL] {}\n  {}({}, {}) failed\n  at {}:{}", CurrentCaseName(),
                 kAssertion, actual_text, expected_text, loc.file_name(), loc.line());
}

Registrar::Registrar(const char* case_name, void (*case_body)()) noexcept {
    if (g_table_size < kMaxCases) {
        g_table[g_table_size] = TestCase{.name = case_name, .body = case_body};
        ++g_table_size;
    }
}

int RegisteredCount() {
    return g_table_size;
}

bool RunSingle(const TestCase& test_case) {
    const int         kFailuresBefore = g_failures;
    const char* const kNameBefore     = g_current_case_name;

    g_current_case_name = test_case.name;
    test_case.body();

    g_current_case_name = kNameBefore;
    const bool kPassed  = g_failures == kFailuresBefore;
    g_failures          = kFailuresBefore;
    return kPassed;
}

int RunAll() {
    int passed = 0;
    int failed = 0;

    std::println("\n=== Cinux Test Runner ===");
    std::println("Running {} case(s)...\n", g_table_size);

    for (int i = 0; i < g_table_size; ++i) {
        if (RunSingle(g_table[i])) {
            std::println("[PASS] {}", g_table[i].name);
            ++passed;
        } else {
            std::println("[FAIL] {}", g_table[i].name);
            ++failed;
        }
    }

    std::println("\n=== Results: {} passed, {} failed ===", passed, failed);
    return failed;
}

}  // namespace cinux::test
