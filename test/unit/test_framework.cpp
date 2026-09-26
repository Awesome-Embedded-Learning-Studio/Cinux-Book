/**
 * @file    test_framework.cpp
 * @brief   The framework tests itself: PASS-counting snapshot semantics.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup test_framework
 */

#include "../framework/framework.hpp"

using cinux::test::g_failures;
using cinux::test::Registrar;
using cinux::test::RegisteredCount;
using cinux::test::RunAll;
using cinux::test::RunSingle;
using cinux::test::TestCase;

namespace {

void must_fail_body() {
    ASSERT_TRUE(false);
}

void must_pass_body() {
    ASSERT_TRUE(true);
}

}  // namespace

TEST("framework: failing case is never counted as passed") {
    const TestCase kFailing{.name = "__injected_fail__", .body = must_fail_body};

    ASSERT_FALSE(RunSingle(kFailing));
}

TEST("framework: passing case is counted as passed") {
    const TestCase kPassing{.name = "__injected_pass__", .body = must_pass_body};

    ASSERT_TRUE(RunSingle(kPassing));
}

TEST("framework: nested RunSingle restores outer counters") {
    const TestCase kFailing{.name = "__injected_fail__", .body = must_fail_body};
    const int      kFailuresBefore = g_failures;

    ASSERT_FALSE(RunSingle(kFailing));

    ASSERT_EQ(g_failures, kFailuresBefore);
}

TEST("framework: registrar appends to the registry") {
    const int       kCountBefore = RegisteredCount();
    const Registrar kInjected{"__late_registered__", must_pass_body};

    ASSERT_EQ(RegisteredCount(), kCountBefore + 1);
}

int main() {
    return RunAll();
}
