/**
 * @file kernel/test/test_dma_buffer.cpp
 * @brief QEMU in-kernel tests for DmaBuffer (M3-1)
 *
 * Exercises the move-only DMA handle: field access, invalid state, move
 * semantics, detach, and RAII release via callback.  No PMM/VMM is used --
 * these are pure value-type tests; DmaPool integration is batch 2.  The fake
 * buffers point at sentinel values (stack vars or unused high addresses) that
 * are never dereferenced; only the handle fields and release hook matter.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/dma/dma_buffer.hpp"

using cinux::drivers::dma::DmaBuffer;

namespace {

// RAII release counter: a DmaBuffer owning via this hook bumps the count when
// destroyed (or reassigned away).  Reset before each test that reads it.
int g_release_count = 0;
void release_counter(const DmaBuffer&) { ++g_release_count; }

}  // namespace

// ============================================================
// Test 1: construction + accessors
// ============================================================

namespace test_dbuf_basic {

void test_construct_and_access() {
    int backing = 42;
    DmaBuffer buf(0x1000ULL, &backing, 4096);
    TEST_ASSERT_TRUE(buf.valid());
    TEST_ASSERT_EQ(buf.phys(), 0x1000ULL);
    TEST_ASSERT_EQ(buf.virt(), static_cast<void*>(&backing));
    TEST_ASSERT_TRUE(buf.size() == 4096);
}

}  // namespace test_dbuf_basic

// ============================================================
// Test 2: default ctor is invalid
// ============================================================

namespace test_dbuf_empty {

void test_default_is_invalid() {
    DmaBuffer buf;
    TEST_ASSERT_FALSE(buf.valid());
    TEST_ASSERT_NULL(buf.virt());
    TEST_ASSERT_TRUE(buf.size() == 0);
}

}  // namespace test_dbuf_empty

// ============================================================
// Test 3: move ctor transfers ownership + empties source
// ============================================================

namespace test_dbuf_move_ctor {

void test_move_ctor_transfers() {
    int backing = 7;
    DmaBuffer a(0x2000ULL, &backing, 8192);
    DmaBuffer b(std::move(a));
    TEST_ASSERT_TRUE(b.valid());
    TEST_ASSERT_EQ(b.phys(), 0x2000ULL);
    TEST_ASSERT_TRUE(b.size() == 8192);
    TEST_ASSERT_FALSE(a.valid());  // source drained
    TEST_ASSERT_NULL(a.virt());
}

}  // namespace test_dbuf_move_ctor

// ============================================================
// Test 4: move assignment transfers + releases the old target
// ============================================================

namespace test_dbuf_move_assign {

void test_move_assign_transfers() {
    int x = 1;
    int y = 2;
    DmaBuffer a(0x10ULL, &x, 16);
    DmaBuffer b(0x20ULL, &y, 32);
    b = std::move(a);
    TEST_ASSERT_TRUE(b.valid());
    TEST_ASSERT_EQ(b.phys(), 0x10ULL);
    TEST_ASSERT_TRUE(b.size() == 16);
    TEST_ASSERT_FALSE(a.valid());
}

}  // namespace test_dbuf_move_assign

// ============================================================
// Test 5: detach surrenders ownership
// ============================================================

namespace test_dbuf_detach {

void test_detach_empties_handle() {
    int backing = 9;
    DmaBuffer buf(0x3000ULL, &backing, 2048);
    uint64_t p = 0;
    void* v = nullptr;
    std::size_t s = 0;
    buf.detach(p, v, s);
    TEST_ASSERT_EQ(p, 0x3000ULL);
    TEST_ASSERT_EQ(v, static_cast<void*>(&backing));
    TEST_ASSERT_TRUE(s == 2048);
    TEST_ASSERT_FALSE(buf.valid());
}

}  // namespace test_dbuf_detach

// ============================================================
// Test 6: RAII release invoked on destruction
// ============================================================

namespace test_dbuf_raii {

void test_destructor_releases() {
    g_release_count = 0;
    {
        DmaBuffer buf(0x4000ULL, reinterpret_cast<void*>(0x4000ULL), 64, release_counter);
        TEST_ASSERT_TRUE(buf.valid());
        TEST_ASSERT_TRUE(g_release_count == 0);  // not yet released
    }  // ~DmaBuffer fires the hook
    TEST_ASSERT_TRUE(g_release_count == 1);
}

}  // namespace test_dbuf_raii

// ============================================================
// Test 7: moved-from source does not release on destruction
// ============================================================

namespace test_dbuf_move_no_double_release {

void test_moved_source_skips_release() {
    g_release_count = 0;
    {
        DmaBuffer a(0x5000ULL, reinterpret_cast<void*>(0x5000ULL), 128, release_counter);
        DmaBuffer b(std::move(a));  // a's release_ cleared
    }  // destroys a (no release) then b (releases)
    TEST_ASSERT_TRUE(g_release_count == 1);  // exactly once, by b
}

}  // namespace test_dbuf_move_no_double_release

// ============================================================
// Entry point
// ============================================================

extern "C" void run_dma_buffer_tests() {
    TEST_SECTION("DmaBuffer Tests (M3-1)");

    RUN_TEST(test_dbuf_basic::test_construct_and_access);
    RUN_TEST(test_dbuf_empty::test_default_is_invalid);
    RUN_TEST(test_dbuf_move_ctor::test_move_ctor_transfers);
    RUN_TEST(test_dbuf_move_assign::test_move_assign_transfers);
    RUN_TEST(test_dbuf_detach::test_detach_empties_handle);
    RUN_TEST(test_dbuf_raii::test_destructor_releases);
    RUN_TEST(test_dbuf_move_no_double_release::test_moved_source_skips_release);

    TEST_SUMMARY();
}
