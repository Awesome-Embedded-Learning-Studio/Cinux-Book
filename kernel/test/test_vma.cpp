/**
 * @file kernel/test/test_vma.cpp
 * @brief QEMU in-kernel tests for LinkedListVMAStore (F2-M1 batch 1)
 *
 * Exercises the VMA store in isolation: ordered insert, find hit/miss, remove
 * (head/middle-split/tail), find_free_area (gap fit, hint inside a node, tail),
 * and adjacent same-flags merge.  No AddressSpace integration (batch 2) and no
 * hardware.  Requires the heap initialised (VMA nodes are heap-allocated).
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/mm/vma.hpp"

using cinux::lib::Error;
using cinux::mm::LinkedListVMAStore;
using cinux::mm::VMA;
using cinux::mm::VmaFlags;

namespace {

// Page-aligned test addresses (kPageSize = 4096).
constexpr uint64_t kA = 0x1000;
constexpr uint64_t kB = 0x2000;
constexpr uint64_t kC = 0x3000;
constexpr uint64_t kD = 0x4000;
constexpr uint64_t kE = 0x5000;
constexpr uint64_t kF = 0x6000;

constexpr VmaFlags kRw = VmaFlags::Read | VmaFlags::Write;
constexpr VmaFlags kRo = VmaFlags::Read;

}  // namespace

// ============================================================
// Test 1: insert keeps the list ordered; find hits and misses
// ============================================================

namespace test_vma_find {

void test_insert_ordered_and_find() {
    LinkedListVMAStore s;
    // Three disjoint ranges, inserted out of address order; the list must come
    // out sorted and stay separate (not adjacent, so no merge).
    TEST_ASSERT_TRUE(s.insert(kC, kD, kRw).ok());  // middle
    TEST_ASSERT_TRUE(s.insert(kA, kB, kRw).ok());  // lowest
    TEST_ASSERT_TRUE(s.insert(kE, kF, kRw).ok());  // highest
    TEST_ASSERT_TRUE(s.count() == 3);

    // find hits the middle node.
    VMA* m = s.find(0x3800);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->start == kC && m->end == kD);

    // find misses: below all, above all, and inside a gap.
    TEST_ASSERT_NULL(s.find(0x0800));  // below [kA, kB)
    TEST_ASSERT_NULL(s.find(0x6800));  // above [kE, kF)
    TEST_ASSERT_NULL(s.find(0x2800));  // in the gap [kB, kC)
}

}  // namespace test_vma_find

// ============================================================
// Test 2: overlap is rejected; bad ranges are rejected
// ============================================================

namespace test_vma_reject {

void test_overlap_and_bad_range() {
    LinkedListVMAStore s;
    TEST_ASSERT_TRUE(s.insert(kA, kC, kRw).ok());

    // Adjacent at kC is fine; interior overlap is rejected.
    TEST_ASSERT_TRUE(s.insert(kC, kD, kRw).ok());
    {
        auto overlap = s.insert(kB, kD, kRw);  // overlaps the [kA, kD) merge
        TEST_ASSERT_FALSE(overlap.ok());
        TEST_ASSERT_TRUE(overlap.error() == Error::AlreadyExists);
    }

    // Bad ranges.
    TEST_ASSERT_FALSE(s.insert(kC, kA, kRw).ok());          // start >= end
    TEST_ASSERT_FALSE(s.insert(0x1001, 0x2000, kRw).ok());  // unaligned
    TEST_ASSERT_FALSE(s.remove(kC, kA).ok());               // start >= end
}

}  // namespace test_vma_reject

// ============================================================
// Test 3: remove untracks; middle removal splits a VMA
// ============================================================

namespace test_vma_remove {

void test_remove_head_middle_tail() {
    LinkedListVMAStore s;
    TEST_ASSERT_TRUE(s.insert(kA, kE, kRw).ok());  // one big [kA, kE)
    TEST_ASSERT_TRUE(s.count() == 1);

    // Remove the middle [kB, kD): splits into [kA, kB) and [kD, kE).
    TEST_ASSERT_TRUE(s.remove(kB, kD).ok());
    TEST_ASSERT_TRUE(s.count() == 2);
    TEST_ASSERT_NOT_NULL(s.find(0x1800));  // in [kA, kB)
    TEST_ASSERT_NULL(s.find(0x2800));      // was the middle, now gone
    TEST_ASSERT_NOT_NULL(s.find(0x4800));  // in [kD, kE)

    // Remove the head node entirely [kA, kB).
    TEST_ASSERT_TRUE(s.remove(kA, kB).ok());
    TEST_ASSERT_TRUE(s.count() == 1);
    TEST_ASSERT_NULL(s.find(0x1800));

    // Remove the remaining tail [kD, kE).
    TEST_ASSERT_TRUE(s.remove(kD, kE).ok());
    TEST_ASSERT_TRUE(s.count() == 0);
    TEST_ASSERT_NULL(s.find(0x4800));
}

void test_remove_preserves_file_offsets_for_right_survivors() {
    LinkedListVMAStore s;
    TEST_ASSERT_TRUE(s.insert(kA, kF, kRw).ok());
    VMA* v = s.find(kA);
    TEST_ASSERT_NOT_NULL(v);
    v->file_offset = 0x9000;

    // Remove the middle [kB, kD): the right survivor starts two pages later in
    // both virtual and file space.
    TEST_ASSERT_TRUE(s.remove(kB, kD).ok());
    VMA* right = s.find(kD);
    TEST_ASSERT_NOT_NULL(right);
    TEST_ASSERT_TRUE(right->start == kD);
    TEST_ASSERT_TRUE(right->file_offset == 0xC000);

    // Trim the left page of the right survivor: file offset advances again.
    TEST_ASSERT_TRUE(s.remove(kD, kE).ok());
    right = s.find(kE);
    TEST_ASSERT_NOT_NULL(right);
    TEST_ASSERT_TRUE(right->start == kE);
    TEST_ASSERT_TRUE(right->file_offset == 0xD000);
}

}  // namespace test_vma_remove

// ============================================================
// Test 4: adjacent same-flags inserts merge; different flags do not
// ============================================================

namespace test_vma_merge {

void test_merge_adjacent_same_flags() {
    LinkedListVMAStore s;
    TEST_ASSERT_TRUE(s.insert(kA, kB, kRw).ok());
    TEST_ASSERT_TRUE(s.insert(kB, kC, kRw).ok());  // adjacent, same flags -> merge
    TEST_ASSERT_TRUE(s.count() == 1);              // coalesced into one node

    VMA* m = s.find(0x1800);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->start == kA && m->end == kC);

    // Different flags do not merge.
    TEST_ASSERT_TRUE(s.insert(kC, kD, kRo).ok());
    TEST_ASSERT_TRUE(s.count() == 2);
}

void test_backed_vma_does_not_merge_by_flags_only() {
    LinkedListVMAStore s;
    TEST_ASSERT_TRUE(s.insert(kA, kB, kRo).ok());
    VMA* left = s.find(kA);
    TEST_ASSERT_NOT_NULL(left);
    left->backing     = reinterpret_cast<cinux::fs::Inode*>(0x1);
    left->file_offset = 0x2000;

    TEST_ASSERT_TRUE(s.insert(kB, kC, kRo).ok());
    TEST_ASSERT_TRUE(s.count() == 2);

    VMA* right = s.find(kB);
    TEST_ASSERT_NOT_NULL(right);
    TEST_ASSERT_TRUE(right->start == kB);
    TEST_ASSERT_TRUE(right->file_offset == 0);

    left->backing = nullptr;
}

}  // namespace test_vma_merge

// ============================================================
// Test 5: find_free_area locates a gap; skips nodes; reaches tail
// ============================================================

namespace test_vma_free_area {

void test_find_free_area() {
    LinkedListVMAStore s;
    // [kA, kB) and [kC, kE); the gap is [kB, kC) = one page.
    TEST_ASSERT_TRUE(s.insert(kA, kB, kRw).ok());
    TEST_ASSERT_TRUE(s.insert(kC, kE, kRw).ok());

    // length 0 is rejected.
    TEST_ASSERT_FALSE(s.find_free_area(kA, 0).ok());

    // Exactly the gap size, hint at kA -> finds kB.
    auto r1 = s.find_free_area(kA, 0x1000);
    TEST_ASSERT_TRUE(r1.ok());
    TEST_ASSERT_TRUE(r1.value() == kB);

    // Hint inside the first node -> skips past it to the gap (kB).
    auto r2 = s.find_free_area(0x1800, 0x1000);
    TEST_ASSERT_TRUE(r2.ok());
    TEST_ASSERT_TRUE(r2.value() == kB);

    // Too large for the gap -> falls through to the tail (kE).
    auto r3 = s.find_free_area(kA, 0x3000);
    TEST_ASSERT_TRUE(r3.ok());
    TEST_ASSERT_TRUE(r3.value() == kE);
}

}  // namespace test_vma_free_area

// ============================================================
// Entry point
// ============================================================

extern "C" void run_vma_tests() {
    TEST_SECTION("VMA Store Tests (F2-M1-1)");

    RUN_TEST(test_vma_find::test_insert_ordered_and_find);
    RUN_TEST(test_vma_reject::test_overlap_and_bad_range);
    RUN_TEST(test_vma_remove::test_remove_head_middle_tail);
    RUN_TEST(test_vma_remove::test_remove_preserves_file_offsets_for_right_survivors);
    RUN_TEST(test_vma_merge::test_merge_adjacent_same_flags);
    RUN_TEST(test_vma_merge::test_backed_vma_does_not_merge_by_flags_only);
    RUN_TEST(test_vma_free_area::test_find_free_area);

    TEST_SUMMARY();
}
