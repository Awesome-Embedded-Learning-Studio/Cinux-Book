/**
 * @file kernel/test/test_futex.cpp
 * @brief QEMU in-kernel tests for the futex syscall (F3-M2 batch 2)
 *
 * Mirrors test_sync.cpp's phantom-task pattern: tasks are built and installed
 * via Scheduler::set_current(), then the test observes wait-queue state.  The
 * whole section runs under a Scheduler::NoRescheduleGuard, so Scheduler::block()
 * marks a task Blocked and returns immediately without context-switching away
 * -- this lets us observe wait-queue state without hanging.  The global futex
 * table persists across tests, so every test wakes its waiters before finishing
 * to avoid stale waiters being matched by a later test at a reused stack address.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_futex.hpp"

using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::percpu;
using cinux::syscall::sys_futex;

namespace {

// futex op numbers (mirror kernel/syscall/sys_futex.cpp).
constexpr uint64_t FUTEX_WAIT        = 0;
constexpr uint64_t FUTEX_WAKE        = 1;
constexpr uint64_t FUTEX_WAIT_BITSET = 9;
constexpr uint64_t FUTEX_WAKE_BITSET = 10;

// errno (literal, sys_signal convention).
constexpr int64_t kEagain = 11;
constexpr int64_t kEfault = 14;
constexpr int64_t kEinval = 22;
constexpr int64_t kEnosys = 38;

// A do-nothing entry point for built tasks (they never truly run).
void dummy_entry() {}

/// Build a named task and install it as the current per-CPU task.
Task* make_current(const char* name) {
    Task* t = TaskBuilder().set_entry(dummy_entry).set_name(name).build();
    Scheduler::set_current(t);
    return t;
}

}  // namespace

namespace test_futex_basic {

void test_wait_val_mismatch_eagain() {
    static uint32_t word = 5;
    Task*           t    = make_current("fx_eagain");
    int64_t r = sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT, /*val=*/6, 0, 0, 0);
    TEST_ASSERT_EQ(r, -kEagain);  // *uaddr(5) != val(6) -> EAGAIN, no block
    TEST_ASSERT_NE(static_cast<int>(t->state), static_cast<int>(TaskState::Blocked));
}

void test_wait_null_uaddr_efault() {
    make_current("fx_efault");
    int64_t r = sys_futex(0, FUTEX_WAIT, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r, -kEfault);
}

void test_invalid_op_enosys() {
    static uint32_t word = 0;
    make_current("fx_enosys");
    int64_t r = sys_futex(reinterpret_cast<uint64_t>(&word), /*op=*/99, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r, -kEnosys);
}

void test_bitset_zero_einval() {
    static uint32_t word = 0;
    make_current("fx_einval");
    int64_t r =
        sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT_BITSET, 0, 0, 0, /*val3=*/0);
    TEST_ASSERT_EQ(r, -kEinval);
}

void test_wake_no_waiters_zero() {
    static uint32_t word = 0;
    make_current("fx_wake_empty");
    int64_t r = sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE, 1, 0, 0, 0);
    TEST_ASSERT_EQ(r, 0);
}

}  // namespace test_futex_basic

namespace test_futex_wake {

void test_wait_blocks_then_wake() {
    static uint32_t word = 5;
    Scheduler::init();

    Task*   waiter = make_current("fx_waiter");
    int64_t r      = sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT, /*val=*/5, 0, 0, 0);
    TEST_ASSERT_EQ(r, 0);  // matched -> enqueued + blocked (block returns, no schedule)
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Blocked));

    // Wake from another "context".
    make_current("fx_waker");
    int64_t w = sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE, 1, 0, 0, 0);
    TEST_ASSERT_EQ(w, 1);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Ready));
}

void test_wake_count_caps() {
    static uint32_t word = 1;
    Scheduler::init();

    Task* w1 = TaskBuilder().set_entry(dummy_entry).set_name("fx_c1").build();
    Task* w2 = TaskBuilder().set_entry(dummy_entry).set_name("fx_c2").build();
    Task* w3 = TaskBuilder().set_entry(dummy_entry).set_name("fx_c3").build();

    Scheduler::set_current(w1);
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT, 1, 0, 0, 0);
    Scheduler::set_current(w2);
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT, 1, 0, 0, 0);
    Scheduler::set_current(w3);
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT, 1, 0, 0, 0);
    TEST_ASSERT_EQ(static_cast<int>(w1->state), static_cast<int>(TaskState::Blocked));
    TEST_ASSERT_EQ(static_cast<int>(w2->state), static_cast<int>(TaskState::Blocked));
    TEST_ASSERT_EQ(static_cast<int>(w3->state), static_cast<int>(TaskState::Blocked));

    make_current("fx_cwaker");
    int64_t w = sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE, /*max=*/2, 0, 0, 0);
    TEST_ASSERT_EQ(w, 2);  // only 2 of 3 woken
    int ready = (w1->state == TaskState::Ready) + (w2->state == TaskState::Ready) +
                (w3->state == TaskState::Ready);
    TEST_ASSERT_EQ(ready, 2);

    // Cleanup: wake the remaining waiter so the global table is empty.
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE, 1, 0, 0, 0);
}

}  // namespace test_futex_wake

namespace test_futex_bitset {

void test_bitset_match_wakes() {
    static uint32_t word = 7;
    Scheduler::init();

    Task*   waiter = make_current("fx_bs1");
    int64_t r =
        sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT_BITSET, 7, 0, 0, /*val3=*/0x1);
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Blocked));

    make_current("fx_bsw1");
    int64_t w =
        sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE_BITSET, 1, 0, 0, /*val3=*/0x1);
    TEST_ASSERT_EQ(w, 1);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Ready));
}

void test_bitset_no_match_keeps_blocked() {
    static uint32_t word = 9;
    Scheduler::init();

    Task* waiter = make_current("fx_bs2");
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAIT_BITSET, 9, 0, 0, /*val3=*/0x1);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Blocked));

    // Wake with a disjoint bitset -> no waiter matches.
    make_current("fx_bsw2");
    int64_t w =
        sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE_BITSET, 1, 0, 0, /*val3=*/0x2);
    TEST_ASSERT_EQ(w, 0);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Blocked));

    // Cleanup: wake with the matching bitset.
    sys_futex(reinterpret_cast<uint64_t>(&word), FUTEX_WAKE_BITSET, 1, 0, 0, /*val3=*/0x1);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Ready));
}

}  // namespace test_futex_bitset

extern "C" void run_futex_tests() {
    TEST_SECTION("Futex Tests (F3-M2-2)");

    // This whole section role-plays tasks on the single harness thread (no real
    // dispatch loop): suppress block()'s reschedule so we observe wait-queue /
    // task state instead of context-switching away to idle.
    Scheduler::NoRescheduleGuard no_resched;

    RUN_TEST(test_futex_basic::test_wait_val_mismatch_eagain);
    RUN_TEST(test_futex_basic::test_wait_null_uaddr_efault);
    RUN_TEST(test_futex_basic::test_invalid_op_enosys);
    RUN_TEST(test_futex_basic::test_bitset_zero_einval);
    RUN_TEST(test_futex_basic::test_wake_no_waiters_zero);

    RUN_TEST(test_futex_wake::test_wait_blocks_then_wake);
    RUN_TEST(test_futex_wake::test_wake_count_caps);

    RUN_TEST(test_futex_bitset::test_bitset_match_wakes);
    RUN_TEST(test_futex_bitset::test_bitset_no_match_keeps_blocked);

    TEST_SUMMARY();
}
