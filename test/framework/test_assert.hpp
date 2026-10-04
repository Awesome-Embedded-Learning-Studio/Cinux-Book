/**
 * @file    test_assert.hpp
 * @brief   The assertion contract and macros both test worlds share.
 *
 * The Check pattern of cinux/assert.hpp, one floor up: ASSERT_* macros
 * evaluate at the call site, then hand any failure to one of the
 * ReportFailure hooks — one declaration here, one definition per world,
 * selected at link time (stderr on the host, COM1 in the kernel). The
 * macro layer stays textual by necessity — the early return must leave
 * the test body — but it no longer knows which world it is in.
 * ASSERT_STREQ stays host-side in framework.hpp: string comparison has
 * no kernel twin. Failure reports carry expression text and position
 * only; values stay behind because the host compares types a kernel
 * cannot even name.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup test_framework
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <source_location>

namespace cinux::test {

/// Global failure counter incremented by ASSERT_* macros.
extern int g_failures;  // NOLINT(bugprone-dynamic-static-initializers) declaration only; both
                        // world definitions are constant-initialized

/**
 * @brief         Name of the case currently executing (for ASSERT_* output).
 *
 * @return        Current case name, or "" outside any case.
 * @since         0.1.0
 * @ingroup       test_framework
 */
const char* CurrentCaseName();

/**
 * @brief         Reports one failed ASSERT_TRUE-family check.
 *
 * @param[in]     expression   Text of the expression that did not hold.
 * @param[in]     loc          Capture point; defaults to the caller, which
 *                             inside a macro expansion is the assertion site.
 * @return        None
 * @note          One declaration, one definition per world, selected at
 *                link time — the Check pattern of cinux/assert.hpp, capture
 *                idiom included.
 * @since         0.1.0
 * @ingroup       test_framework
 */
void ReportCheckFailure(const char*          expression,
                        std::source_location loc = std::source_location::current());

/**
 * @brief         Reports one failed ASSERT_EQ or ASSERT_NE comparison.
 *
 * @param[in]     actual_text      Text of the actual-value expression.
 * @param[in]     expected_text    Text of the expected-value expression.
 * @param[in]     expected_equal   true for ASSERT_EQ (wanted equal), false
 *                                 for ASSERT_NE (wanted different).
 * @param[in]     loc              Capture point; defaults to the caller.
 * @return        None
 * @note          Values themselves stay at the call site: the worlds
 *                cannot agree on a type that carries them.
 * @since         0.1.0
 * @ingroup       test_framework
 */
void ReportCompareFailure(const char* actual_text, const char* expected_text, bool expected_equal,
                          std::source_location loc = std::source_location::current());

}  // namespace cinux::test

// ============================================================
// ASSERT_* — record a failure, report it, and return out of the case.
// Macro (not function) by necessity: the early `return` must leave the
// test body, which only textual expansion can do. Position capture and
// reporting are link-time world selection via the ReportFailure hooks,
// whose default source_location argument lands on this very line.
// ============================================================
#define ASSERT_TRUE(expression)                                                                    \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::cinux::test::ReportCheckFailure(#expression);                                        \
            ++::cinux::test::g_failures;                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_FALSE(expression) ASSERT_TRUE(!(expression))

#define ASSERT_EQ(actual, expected)                                                                \
    do {                                                                                           \
        const auto& kActualValue   = (actual);                                                     \
        const auto& kExpectedValue = (expected);                                                   \
        if (!(kActualValue == kExpectedValue)) {                                                   \
            ::cinux::test::ReportCompareFailure(#actual, #expected, true);                         \
            ++::cinux::test::g_failures;                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NE(actual, expected)                                                                \
    do {                                                                                           \
        const auto& kActualValue   = (actual);                                                     \
        const auto& kExpectedValue = (expected);                                                   \
        if (kActualValue == kExpectedValue) {                                                      \
            ::cinux::test::ReportCompareFailure(#actual, #expected, false);                        \
            ++::cinux::test::g_failures;                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NULL(pointer)     ASSERT_TRUE((pointer) == nullptr)
#define ASSERT_NOT_NULL(pointer) ASSERT_TRUE((pointer) != nullptr)

#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
