/**
 * @file kernel/test/test_prdt_builder.cpp
 * @brief QEMU in-kernel tests for PrdtBuilder (M3-3)
 *
 * Pure-logic tests (no PMM/VMM): segment splitting at a per-segment limit,
 * multi-buffer accumulation, fixed-capacity overflow, the DmaBuffer adapter,
 * fits() pre-checks, and the max_segment==0 guard (no infinite loop).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/dma/prdt_builder.hpp"

using cinux::drivers::dma::DmaBuffer;
using cinux::drivers::dma::PrdtBuilder;

// ============================================================
// Test 1: a small single segment fits whole
// ============================================================

namespace test_prdt_single {

void test_single_segment() {
    PrdtBuilder<4> b;
    TEST_ASSERT_TRUE(b.add(0x1000, 512, 4096) == 1);
    TEST_ASSERT_TRUE(b.count() == 1);
    TEST_ASSERT_EQ(b.segment(0).phys, 0x1000ULL);
    TEST_ASSERT_EQ(b.segment(0).size, 512ULL);
    TEST_ASSERT_FALSE(b.full());
}

}  // namespace test_prdt_single

// ============================================================
// Test 2: a range larger than the limit is split
// ============================================================

namespace test_prdt_split {

void test_splits_at_max() {
    PrdtBuilder<8> b;
    // 10 KiB at 4 KiB max -> 3 segments: 4096, 4096, 2048
    TEST_ASSERT_TRUE(b.add(0x0, 10240, 4096) == 3);
    TEST_ASSERT_TRUE(b.count() == 3);
    TEST_ASSERT_EQ(b.segment(0).phys, 0x0ULL);
    TEST_ASSERT_EQ(b.segment(0).size, 4096ULL);
    TEST_ASSERT_EQ(b.segment(1).phys, 4096ULL);
    TEST_ASSERT_EQ(b.segment(1).size, 4096ULL);
    TEST_ASSERT_EQ(b.segment(2).phys, 8192ULL);
    TEST_ASSERT_EQ(b.segment(2).size, 2048ULL);
}

}  // namespace test_prdt_split

// ============================================================
// Test 3: multiple adds accumulate
// ============================================================

namespace test_prdt_multi {

void test_multiple_adds() {
    PrdtBuilder<8> b;
    b.add(0x1000, 4096, 4096);
    b.add(0x9000, 4096, 4096);
    TEST_ASSERT_TRUE(b.count() == 2);
    TEST_ASSERT_EQ(b.segment(0).phys, 0x1000ULL);
    TEST_ASSERT_EQ(b.segment(1).phys, 0x9000ULL);
}

}  // namespace test_prdt_multi

// ============================================================
// Test 4: a full table drops further adds (no overflow)
// ============================================================

namespace test_prdt_full {

void test_full_stops() {
    PrdtBuilder<2> b;
    b.add(0x0, 4096, 4096);
    b.add(0x1000, 4096, 4096);  // fills the table
    TEST_ASSERT_TRUE(b.full());
    TEST_ASSERT_TRUE(b.add(0x2000, 4096, 4096) == 0);  // dropped
    TEST_ASSERT_TRUE(b.count() == 2);
}

}  // namespace test_prdt_full

// ============================================================
// Test 5: add_buffer splits a DmaBuffer
// ============================================================

namespace test_prdt_buffer {

void test_add_buffer() {
    PrdtBuilder<8> b;
    int      backing = 0;
    DmaBuffer buf(0x5000, &backing, 8192);
    TEST_ASSERT_TRUE(b.add_buffer(buf, 4096) == 2);
    TEST_ASSERT_TRUE(b.count() == 2);
    TEST_ASSERT_EQ(b.segment(0).phys, 0x5000ULL);
    TEST_ASSERT_EQ(b.segment(0).size, 4096ULL);
    TEST_ASSERT_EQ(b.segment(1).phys, 0x6000ULL);  // 0x5000 + 4096
    TEST_ASSERT_EQ(b.segment(1).size, 4096ULL);
}

}  // namespace test_prdt_buffer

// ============================================================
// Test 6: fits() capacity pre-check
// ============================================================

namespace test_prdt_fits {

void test_fits() {
    PrdtBuilder<4> b;
    TEST_ASSERT_TRUE(b.fits(4096, 4096));    // 1 seg, room for 4
    TEST_ASSERT_TRUE(b.fits(16384, 4096));   // 4 seg, exactly fits
    TEST_ASSERT_FALSE(b.fits(20480, 4096));  // 5 seg > 4
    b.add(0x0, 4096, 4096);                   // 1 slot used
    TEST_ASSERT_FALSE(b.fits(16384, 4096));  // need 4, have 3 left
}

}  // namespace test_prdt_fits

// ============================================================
// Test 7: max_segment == 0 is a no-op (no infinite loop)
// ============================================================

namespace test_prdt_zero_max {

void test_zero_max_no_loop() {
    PrdtBuilder<4> b;
    TEST_ASSERT_TRUE(b.add(0x0, 4096, 0) == 0);
    TEST_ASSERT_TRUE(b.count() == 0);
    TEST_ASSERT_FALSE(b.fits(1, 0));  // size > 0 with max 0 -> no fit
}

}  // namespace test_prdt_zero_max

// ============================================================
// Entry point
// ============================================================

extern "C" void run_prdt_builder_tests() {
    TEST_SECTION("PrdtBuilder Tests (M3-3)");

    RUN_TEST(test_prdt_single::test_single_segment);
    RUN_TEST(test_prdt_split::test_splits_at_max);
    RUN_TEST(test_prdt_multi::test_multiple_adds);
    RUN_TEST(test_prdt_full::test_full_stops);
    RUN_TEST(test_prdt_buffer::test_add_buffer);
    RUN_TEST(test_prdt_fits::test_fits);
    RUN_TEST(test_prdt_zero_max::test_zero_max_no_loop);

    TEST_SUMMARY();
}
