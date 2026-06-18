/**
 * @file kernel/test/test_backtrace.cpp
 * @brief QEMU in-kernel tests for frame-pointer backtrace (FO batch 2)
 *
 * Exercises backtrace_capture() (the pure, array-filling core): a nested
 * noinline call chain must yield several frames, the depth cap is honoured,
 * and a null/empty starting pointer stops cleanly without faulting.  The
 * symbolization-printing path (backtrace_from) is observed live during the
 * panic-handler integration (batch 3), not asserted here.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/backtrace.hpp"

using cinux::arch::backtrace_capture;

namespace {

// Captured frames shared by the nested helpers below.  volatile so the
// optimizer cannot elide the writes.
volatile uint64_t g_addrs[32];
volatile size_t   g_count;

// A small noinline call chain.  With -fno-omit-frame-pointer (batch 0) each
// function has a real frame, so walking from leaf()'s RBP must cross at least
// middle -> top -> the test function.
__attribute__((noinline)) void leaf() {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    g_count = backtrace_capture(rbp, const_cast<uint64_t*>(g_addrs), 32);
}

__attribute__((noinline)) void middle() {
    leaf();
    __asm__ volatile("" ::: "memory");  // block tail-call so a real frame is kept
}

__attribute__((noinline)) void top() {
    middle();
    __asm__ volatile("" ::: "memory");  // block tail-call so a real frame is kept
}

}  // namespace

// ============================================================
// Test 1: a real call chain walks multiple frames
// ============================================================

namespace test_backtrace_depth {

void test_walks_multiple_frames() {
    top();
    // leaf -> middle -> top -> test -> runner: at least 3 frames above leaf.
    TEST_ASSERT_GE(g_count, (size_t)3);
}

}  // namespace test_backtrace_depth

// ============================================================
// Test 2: the depth cap is honoured
// ============================================================

namespace test_backtrace_limit {

void test_respects_max() {
    uint64_t a[8];
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    size_t n = backtrace_capture(rbp, a, 2);  // tiny cap
    TEST_ASSERT_LE(n, (size_t)2);
}

}  // namespace test_backtrace_limit

// ============================================================
// Test 3: a null / empty start pointer stops cleanly (no fault)
// ============================================================

namespace test_backtrace_bad {

void test_zero_rbp_stops() {
    uint64_t a[8];
    TEST_ASSERT_EQ(backtrace_capture(0, a, 8), (size_t)0);
}

void test_null_buffer() {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    TEST_ASSERT_EQ(backtrace_capture(rbp, nullptr, 8), (size_t)0);
}

}  // namespace test_backtrace_bad

// ============================================================
// Entry point
// ============================================================

extern "C" void run_backtrace_tests() {
    TEST_SECTION("Backtrace Tests (FO)");

    RUN_TEST(test_backtrace_depth::test_walks_multiple_frames);
    RUN_TEST(test_backtrace_limit::test_respects_max);
    RUN_TEST(test_backtrace_bad::test_zero_rbp_stops);
    RUN_TEST(test_backtrace_bad::test_null_buffer);

    TEST_SUMMARY();
}
