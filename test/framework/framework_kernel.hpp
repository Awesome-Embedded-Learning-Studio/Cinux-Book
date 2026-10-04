/**
 * @file    framework_kernel.hpp
 * @brief   Cinux lightweight test framework (kernel world).
 *
 * The kernel is a zero-global-construction world, so registration cannot
 * ride static constructors: TEST() places each case record in the
 * .test_cases input section, the linker script fences that range with
 * g_test_start/g_test_end, and RunKernelTests() walks it at boot. The
 * ASSERT_* family is shared with the host through test_assert.hpp; this
 * header carries only the kernel's registration and runner faces. A
 * non-empty run prints per-case results on COM1 and terminates QEMU via
 * isa-debug-exit (port 0xF4, exit code (code<<1)+1 with code = failures
 * + 1, so all-pass exits QEMU with 3), which CTest asserts.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup test_framework
 * @copyright Copyright (c) 2026
 */

#pragma once


namespace cinux::test {

/**
 * @brief         Runs the .test_cases table, then leaves QEMU.
 *
 * @return        None
 * @note          An empty table means a shipping image: the call returns
 *                at once and the caller parks as always. A non-empty run
 *                prints per-case results on COM1 and terminates QEMU via
 *                isa-debug-exit with code = failures + 1.
 * @since         0.1.0
 * @ingroup       test_framework
 */
void RunKernelTests();

}  // namespace cinux::test

// ============================================================
// TEST() — place a code block into the .test_cases linker section.
// __LINE__ (not __COUNTER__): the counter increments at every occurrence,
// one expansion would name three different functions; the line number is
// stable inside a single expansion.
// ============================================================
#define CINUX_TEST_CAT2(a, b) a##b
#define CINUX_TEST_CAT(a, b)  CINUX_TEST_CAT2(a, b)

#define TEST(case_name)                                                                            \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)(); /* NOLINT(misc-use-anonymous-namespace) */             \
    __attribute__((used, section(".test_cases"))) static const ::cinux::test::TestCase             \
        CINUX_TEST_CAT(cinux_test_case_, __LINE__){case_name,                                      \
                                                   CINUX_TEST_CAT(cinux_test_fn_, __LINE__)};      \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)() /* NOLINT(misc-use-anonymous-namespace) */
