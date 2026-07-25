/**
 * @file kernel/test/test_brk.cpp
 * @brief QEMU in-kernel tests for sys_brk (F2-M3 batch 1)
 *
 * brk is lazy: sys_brk only moves brk_current, so the tests verify the
 * query/extend/bounds logic against a temporary Task (no AddressSpace needed).
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_brk.hpp"

using cinux::arch::USER_BRK_BASE;
using cinux::arch::USER_BRK_MAX;
using cinux::syscall::sys_brk;

namespace {

/// RAII: install @p task as current, restore the previous on destruction.
struct CurrentTaskGuard {
    cinux::proc::Task* prev;
    explicit CurrentTaskGuard(cinux::proc::Task* task) : prev(cinux::proc::Scheduler::current()) {
        cinux::proc::Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { cinux::proc::Scheduler::set_current(prev); }
};

}  // namespace

// ============================================================
// Test 1: brk(0) queries; brk(addr) extends; out-of-range is ignored
// ============================================================

namespace test_brk_basic {

void test_brk_query_extend_bounds() {
    cinux::proc::Task tmp{};
    tmp.brk_initial = USER_BRK_BASE;
    tmp.brk_current = USER_BRK_BASE;
    tmp.brk_max     = USER_BRK_MAX;
    CurrentTaskGuard guard(&tmp);

    // addr == 0 returns the current break unchanged.
    TEST_ASSERT_TRUE(sys_brk(0, 0, 0, 0, 0, 0) == static_cast<int64_t>(USER_BRK_BASE));

    // A valid extension moves the break and returns the new value.
    int64_t r = sys_brk(USER_BRK_BASE + 4096, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(r == static_cast<int64_t>(USER_BRK_BASE + 4096));
    TEST_ASSERT_TRUE(tmp.brk_current == USER_BRK_BASE + 4096);

    // Below brk_initial is ignored -- break unchanged, current returned.
    TEST_ASSERT_TRUE(sys_brk(USER_BRK_BASE - 1, 0, 0, 0, 0, 0) ==
                     static_cast<int64_t>(tmp.brk_current));
    TEST_ASSERT_TRUE(tmp.brk_current == USER_BRK_BASE + 4096);

    // Above brk_max is ignored.
    TEST_ASSERT_TRUE(sys_brk(USER_BRK_MAX + 1, 0, 0, 0, 0, 0) ==
                     static_cast<int64_t>(tmp.brk_current));
    TEST_ASSERT_TRUE(tmp.brk_current == USER_BRK_BASE + 4096);
}

}  // namespace test_brk_basic

// ============================================================
// Entry point
// ============================================================

extern "C" void run_brk_tests() {
    TEST_SECTION("brk Tests (F2-M3-1)");

    RUN_TEST(test_brk_basic::test_brk_query_extend_bounds);

    TEST_SUMMARY();
}
