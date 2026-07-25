/**
 * @file kernel/test/test_buddy.cpp
 * @brief QEMU in-kernel tests for the BuddyAllocator (F2-M7 batch 1)
 *
 * Exercises the buddy allocator in isolation over 256 page indices.  The
 * bitmap free-list tracks blocks purely by index (it never touches the pages
 * themselves), so no real memory mapping is needed.  No PMM or scheduler is
 * involved.
 *
 * Validates: init stats, single-page alloc/free, contiguous order-N blocks,
 * bulk alloc/free with exact count restoration, double-free / out-of-range
 * no-ops, OOM, and exhaustive alloc-all/free-all (no leak under exhaustion).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/buddy.hpp"

using cinux::arch::KERNEL_VMA;
using cinux::arch::PAGE_SIZE;
using cinux::mm::BuddyAllocator;

namespace {

/// 256 pages = 1 MiB, a clean power of two so the whole region is one order-8
/// block initially -- exercising split/coalesce as tests alloc smaller orders.
constexpr uint64_t kFakePages = 256;
uint8_t            g_order[kFakePages];
uint8_t            g_bitmap[1024];  // per-order free bitmaps (bitmap_bytes(256) is tiny)

/// Initialise a fresh buddy over 256 page indices (all free).  base_phys is
/// recorded but unused -- the bitmap free-list works purely on page indices.
void setup(BuddyAllocator& b) {
    b.init(0, kFakePages, g_order, g_bitmap);
    b.mark_free_region(0, kFakePages);
}

}  // namespace

// ============================================================
// Test 1: init statistics
// ============================================================

namespace test_buddy_init {

void test_init_stats() {
    BuddyAllocator b;
    setup(b);
    TEST_ASSERT_EQ(b.total_pages(), kFakePages);
    TEST_ASSERT_EQ(b.free_pages(), kFakePages);
}

}  // namespace test_buddy_init

// ============================================================
// Test 2: single-page alloc/free cycle (order 0)
// ============================================================

namespace test_buddy_single {

void test_alloc_free_single() {
    BuddyAllocator b;
    setup(b);
    uint64_t initial = b.free_pages();

    uint64_t p = b.alloc_order(0);
    TEST_ASSERT_NE(p, BuddyAllocator::kInvalidPage);
    TEST_ASSERT_EQ(b.free_pages(), initial - 1);

    b.free(p);
    TEST_ASSERT_EQ(b.free_pages(), initial);
}

}  // namespace test_buddy_single

// ============================================================
// Test 3: contiguous order-N block (4 pages, head 4-aligned)
// ============================================================

namespace test_buddy_contiguous {

void test_alloc_order_contiguous() {
    BuddyAllocator b;
    setup(b);
    uint64_t initial = b.free_pages();

    uint64_t p = b.alloc_order(2);  // 4 pages
    TEST_ASSERT_NE(p, BuddyAllocator::kInvalidPage);
    TEST_ASSERT_EQ(p % 4, 0ULL);  // order-2 head is 4-page aligned
    TEST_ASSERT_EQ(b.free_pages(), initial - 4);

    b.free(p);
    TEST_ASSERT_EQ(b.free_pages(), initial);
}

}  // namespace test_buddy_contiguous

// ============================================================
// Test 4: bulk single-page alloc/free restores the count exactly
// ============================================================

namespace test_buddy_bulk {

void test_bulk_alloc_free() {
    BuddyAllocator b;
    setup(b);
    uint64_t initial = b.free_pages();

    constexpr uint64_t N = 16;
    uint64_t           pages[N];
    for (uint64_t i = 0; i < N; i++) {
        pages[i] = b.alloc_order(0);
        TEST_ASSERT_NE(pages[i], BuddyAllocator::kInvalidPage);
    }
    TEST_ASSERT_EQ(b.free_pages(), initial - N);

    for (uint64_t i = 0; i < N; i++) {
        b.free(pages[i]);
    }
    TEST_ASSERT_EQ(b.free_pages(), initial);
}

}  // namespace test_buddy_bulk

// ============================================================
// Test 5: double-free / out-of-range / interior are no-ops
// ============================================================

namespace test_buddy_edge {

void test_double_free_noop() {
    BuddyAllocator b;
    setup(b);
    uint64_t p = b.alloc_order(0);
    b.free(p);
    uint64_t after = b.free_pages();

    b.free(p);  // double free
    TEST_ASSERT_EQ(b.free_pages(), after);

    b.free(BuddyAllocator::kInvalidPage);  // sentinel index
    TEST_ASSERT_EQ(b.free_pages(), after);

    b.free(kFakePages + 10);  // out of range
    TEST_ASSERT_EQ(b.free_pages(), after);
}

void test_free_interior_noop() {
    BuddyAllocator b;
    setup(b);
    uint64_t p     = b.alloc_order(2);  // block [p, p+4)
    uint64_t after = b.free_pages();

    b.free(p + 1);  // interior page, not a head
    b.free(p + 2);
    TEST_ASSERT_EQ(b.free_pages(), after);

    b.free(p);  // the real head still frees correctly
    TEST_ASSERT_EQ(b.free_pages(), after + 4);
}

}  // namespace test_buddy_edge

// ============================================================
// Test 6: OOM returns kInvalidPage without corrupting counts
// ============================================================

namespace test_buddy_oom {

void test_exhaustion_then_oom() {
    BuddyAllocator b;
    setup(b);

    // Drain every page one at a time.
    uint64_t allocated = 0;
    while (true) {
        uint64_t p = b.alloc_order(0);
        if (p == BuddyAllocator::kInvalidPage) {
            break;
        }
        allocated++;
    }
    TEST_ASSERT_EQ(allocated, kFakePages);
    TEST_ASSERT_EQ(b.free_pages(), 0ULL);

    // One more must fail cleanly.
    TEST_ASSERT_EQ(b.alloc_order(0), BuddyAllocator::kInvalidPage);

    // Large-order alloc also fails when only order-0 pages could be formed
    // (the pool is empty).
    TEST_ASSERT_EQ(b.alloc_order(4), BuddyAllocator::kInvalidPage);
}

}  // namespace test_buddy_oom

// ============================================================
// Test 7: alloc-all then free-all leaves no leak (coalescing soundness)
// ============================================================

namespace test_buddy_coalesce {

void test_drain_and_restore() {
    BuddyAllocator b;
    setup(b);
    uint64_t initial = b.free_pages();

    // Allocate the whole pool as order-0 pages, then return them in reverse.
    constexpr uint64_t kCap = 256;
    uint64_t           pages[kCap];
    uint64_t           n = 0;
    for (uint64_t i = 0; i < kCap; i++) {
        uint64_t p = b.alloc_order(0);
        if (p == BuddyAllocator::kInvalidPage) {
            break;
        }
        pages[n++] = p;
    }
    TEST_ASSERT_EQ(n, kFakePages);
    TEST_ASSERT_EQ(b.free_pages(), 0ULL);

    for (uint64_t i = 0; i < n; i++) {
        b.free(pages[n - 1 - i]);  // reverse order stresses coalescing paths
    }
    TEST_ASSERT_EQ(b.free_pages(), initial);

    // After full coalescing the pool must satisfy the original order-8 block again.
    uint64_t big = b.alloc_order(8);
    TEST_ASSERT_NE(big, BuddyAllocator::kInvalidPage);
    TEST_ASSERT_EQ(big, 0ULL);  // whole region reformed into one block at index 0
    b.free(big);
    TEST_ASSERT_EQ(b.free_pages(), initial);
}

}  // namespace test_buddy_coalesce

// ============================================================
// Entry point
// ============================================================

extern "C" void run_buddy_tests() {
    TEST_SECTION("Buddy Allocator Tests (F2-M7)");

    RUN_TEST(test_buddy_init::test_init_stats);
    RUN_TEST(test_buddy_single::test_alloc_free_single);
    RUN_TEST(test_buddy_contiguous::test_alloc_order_contiguous);
    RUN_TEST(test_buddy_bulk::test_bulk_alloc_free);
    RUN_TEST(test_buddy_edge::test_double_free_noop);
    RUN_TEST(test_buddy_edge::test_free_interior_noop);
    RUN_TEST(test_buddy_oom::test_exhaustion_then_oom);
    RUN_TEST(test_buddy_coalesce::test_drain_and_restore);

    TEST_SUMMARY();
}
