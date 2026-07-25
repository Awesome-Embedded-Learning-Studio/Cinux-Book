/**
 * @file kernel/test/test_concurrent_ring_buffer.cpp
 * @brief QEMU in-kernel tests for ConcurrentRingBuffer (MPSC, IRQ-safe)
 *
 * Verifies the IRQ-safe RingBuffer wrapper: FIFO order, full/empty
 * semantics, batch ops with wrap-around, clear, and capacity.  Each
 * operation takes irq_guard() internally; true multi-CPU contention is
 * not exercisable on the single-CPU QEMU test rig, but the locking path
 * (cli/sti + Spinlock) is exercised on every call.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/lib/concurrent_ring_buffer.hpp"

using cinux::lib::ConcurrentRingBuffer;

// ============================================================
// Test 1: FIFO push/pop
// ============================================================

namespace test_crb_fifo {

void test_push_pop_fifo() {
    ConcurrentRingBuffer<int, 8> rb;
    TEST_ASSERT_TRUE(rb.empty());

    TEST_ASSERT_TRUE(rb.push(10));
    TEST_ASSERT_TRUE(rb.push(20));
    TEST_ASSERT_TRUE(rb.push(30));
    TEST_ASSERT_TRUE(rb.size() == 3);

    int out = 0;
    TEST_ASSERT_TRUE(rb.pop(out));
    TEST_ASSERT_EQ(out, 10);
    TEST_ASSERT_TRUE(rb.pop(out));
    TEST_ASSERT_EQ(out, 20);
    TEST_ASSERT_TRUE(rb.pop(out));
    TEST_ASSERT_EQ(out, 30);

    TEST_ASSERT_FALSE(rb.pop(out));  // empty
    TEST_ASSERT_TRUE(rb.empty());
}

}  // namespace test_crb_fifo

// ============================================================
// Test 2: full -> push returns false (drop)
// ============================================================

namespace test_crb_full {

void test_full_push_returns_false() {
    ConcurrentRingBuffer<int, 3> rb;
    TEST_ASSERT_TRUE(rb.push(1));
    TEST_ASSERT_TRUE(rb.push(2));
    TEST_ASSERT_TRUE(rb.push(3));
    TEST_ASSERT_TRUE(rb.full());

    TEST_ASSERT_FALSE(rb.push(4));  // dropped
    TEST_ASSERT_TRUE(rb.size() == 3);
}

}  // namespace test_crb_full

// ============================================================
// Test 3: batch push/pop + wrap-around
// ============================================================

namespace test_crb_batch {

void test_batch_and_wrap() {
    ConcurrentRingBuffer<int, 4> rb;

    const int in[] = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(rb.push_batch(in, 4) == 4);  // fill exactly
    TEST_ASSERT_TRUE(rb.full());
    TEST_ASSERT_TRUE(rb.push_batch(in, 1) == 0);  // full -> none pushed

    int out[4] = {0, 0, 0, 0};
    TEST_ASSERT_TRUE(rb.pop_batch(out, 4) == 4);
    TEST_ASSERT_EQ(out[0], 1);
    TEST_ASSERT_EQ(out[3], 4);

    // Wrap-around: refill after draining (head_/tail_ advanced)
    TEST_ASSERT_TRUE(rb.push_batch(in, 4) == 4);
    int out2[4] = {0, 0, 0, 0};
    TEST_ASSERT_TRUE(rb.pop_batch(out2, 4) == 4);
    TEST_ASSERT_EQ(out2[0], 1);
    TEST_ASSERT_EQ(out2[3], 4);
}

}  // namespace test_crb_batch

// ============================================================
// Test 4: clear
// ============================================================

namespace test_crb_clear {

void test_clear_empties() {
    ConcurrentRingBuffer<int, 8> rb;
    rb.push(1);
    rb.push(2);
    TEST_ASSERT_TRUE(rb.size() == 2);

    rb.clear();
    TEST_ASSERT_TRUE(rb.empty());
    TEST_ASSERT_TRUE(rb.size() == 0);

    int out = 0;
    TEST_ASSERT_FALSE(rb.pop(out));
}

}  // namespace test_crb_clear

// ============================================================
// Test 5: capacity (compile-time constant)
// ============================================================

namespace test_crb_capacity {

void test_capacity_constant() {
    ConcurrentRingBuffer<int, 16> rb;
    TEST_ASSERT_TRUE(rb.capacity() == 16);
}

}  // namespace test_crb_capacity

// ============================================================
// Entry point
// ============================================================

extern "C" void run_concurrent_ring_buffer_tests() {
    TEST_SECTION("ConcurrentRingBuffer Tests (M2-1)");

    RUN_TEST(test_crb_fifo::test_push_pop_fifo);
    RUN_TEST(test_crb_full::test_full_push_returns_false);
    RUN_TEST(test_crb_batch::test_batch_and_wrap);
    RUN_TEST(test_crb_clear::test_clear_empties);
    RUN_TEST(test_crb_capacity::test_capacity_constant);

    TEST_SUMMARY();
}
