/**
 * @file    framework.hpp
 * @brief   Cinux lightweight test framework (host world).
 *
 * Registration happens at static-construction time via Registrar objects
 * emitted by the TEST() macro; RunAll() executes every registered case and
 * reports PASS/FAIL per case (snapshot semantics: a case passes only when
 * it produced no failure), returning the failure count for the process
 * exit code. The ASSERT_* family and its ReportFailure hooks are shared
 * with the kernel world through test_assert.hpp; the kernel twin of this
 * whole file family is framework_kernel.hpp / framework_kernel.cpp, and
 * both worlds walk the one record from test_case.hpp (link-time
 * selection, one contract per world — see .notes/v2-arch.md).
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 3.0
 * @since   0.1.0
 * @ingroup test_framework
 */

#pragma once

#include <cstdio>   // NOLINT(misc-include-cleaner) stderr is used inside ASSERT_STREQ
#include <cstring>  // NOLINT(misc-include-cleaner) strcmp is used inside ASSERT_STREQ
#include <print>    // NOLINT(misc-include-cleaner) println is used inside ASSERT_STREQ

#include "test_assert.hpp"  // NOLINT(misc-include-cleaner) hooks used inside ASSERT_STREQ
#include "test_case.hpp"

namespace cinux::test {

/**
 * @brief         Registers one test case at static-construction time.
 *
 * @param[in]     case_name   Name shown in the runner output.
 * @param[in]     case_body   Function to register.
 * @return        None
 * @note          Constructing a Registrar at run time also registers the
 *                case (used by the framework's own self-test).
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       test_framework
 */
class Registrar {
public:
    Registrar(const char* case_name, void (*case_body)()) noexcept;
};

/**
 * @brief         Runs one case in isolation; used by RunAll() and by the
 *                framework self-test.
 *
 * @param[in]     test_case   The case to execute.
 * @return        true when the case produced no failure.
 * @note          Global failure counters are snapshot and restored, so a
 *                nested RunSingle() inside a running case stays side-effect
 *                free with respect to the outer run.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       test_framework
 */
bool RunSingle(const TestCase& test_case);

/**
 * @brief         Runs every registered case, printing per-case results.
 *
 * @return        Number of failed cases; intended as the process exit code.
 * @note          A case counts as PASS only if the global failure counter
 *                did not move while it ran (the v1 framework once labelled
 *                every case PASS unconditionally — that bug is the reason
 *                this snapshot rule exists and is self-tested).
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       test_framework
 */
int RunAll();

/**
 * @brief         Number of registered cases so far.
 *
 * @return        Registration count.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       test_framework
 */
int RegisteredCount();

// ============================================================
// TEST() — register a code block as a case.
// __LINE__ (not __COUNTER__): the counter increments at every occurrence,
// one expansion would name three different functions; the line number is
// stable inside a single expansion.
// ============================================================
#define CINUX_TEST_CAT2(a, b) a##b
#define CINUX_TEST_CAT(a, b)  CINUX_TEST_CAT2(a, b)

#define TEST(case_name)                                                                            \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)(); /* NOLINT(misc-use-anonymous-namespace) */             \
    static const ::cinux::test::Registrar CINUX_TEST_CAT(cinux_test_reg_, __LINE__){               \
        case_name, CINUX_TEST_CAT(cinux_test_fn_, __LINE__)};                                      \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)() /* NOLINT(misc-use-anonymous-namespace) */

// ============================================================
// ASSERT_STREQ — the one host-only assertion: string comparison and
// string printing have no kernel twin. Everything else lives in
// test_assert.hpp, shared with the kernel world.
// ============================================================
#define ASSERT_STREQ(actual, expected)                                                             \
    do {                                                                                           \
        const char* kActualText   = (actual);                                                      \
        const char* kExpectedText = (expected);                                                    \
        if (std::strcmp(kActualText, kExpectedText) != 0) {                                        \
            std::println(stderr,                                                                   \
                         "[FAIL] {}\n  ASSERT_STREQ({}, {}) failed: got \"{}\", "                  \
                         "want \"{}\"\n  at {}:{}",                                                \
                         ::cinux::test::CurrentCaseName(), #actual, #expected, kActualText,        \
                         kExpectedText, __FILE__, __LINE__);                                       \
            ++::cinux::test::g_failures;                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

}  // namespace cinux::test
