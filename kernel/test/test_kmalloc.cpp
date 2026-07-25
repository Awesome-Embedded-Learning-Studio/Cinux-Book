/**
 * @file kernel/test/test_kmalloc.cpp
 * @brief QEMU in-kernel tests for kmalloc/kfree (F2-M7b batch 2)
 *
 * kmalloc routes small requests (effective size <= kSlabMaxObj) to the slab
 * caches and large requests to whole buddy pages exposed via the direct map.
 * Covers: routing by address window, the 2048/2049 boundary, round-trips with
 * zeroing on both paths, alignment, operator-new integration, free-page
 * accounting for the large path, nullptr/invalid free safety, and a churn loop.
 */

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"

using cinux::arch::DIRECT_MAP_BASE;
using cinux::arch::KMEM_SLAB_BASE;
using cinux::arch::KMEM_SLAB_SIZE;
using cinux::mm::g_pmm;
using cinux::mm::kfree;
using cinux::mm::kmalloc;
using cinux::mm::kSlabMaxObj;

namespace {

bool in_slab_window(void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= KMEM_SLAB_BASE && a < KMEM_SLAB_BASE + KMEM_SLAB_SIZE;
}

bool in_direct_map(void* p) {
    return reinterpret_cast<uintptr_t>(p) >= DIRECT_MAP_BASE;
}

void fill(void* p, size_t n, uint8_t v) {
    auto* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        b[i] = v;
    }
}

bool is_zero(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

bool check(const void* p, size_t n, uint8_t v) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        if (b[i] != v) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ============================================================
// Test 1: small -> slab window, large -> direct-map window
// ============================================================

namespace test_kmalloc_routing {

void test_routing() {
    void* s = kmalloc(64);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(in_slab_window(s));
    kfree(s);

    void* l = kmalloc(4096);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_TRUE(in_direct_map(l));
    kfree(l);
}

}  // namespace test_kmalloc_routing

// ============================================================
// Test 2: exact 2048 / 2049 boundary
// ============================================================

namespace test_kmalloc_boundary {

void test_boundary() {
    void* a = kmalloc(kSlabMaxObj);  // 2048 -> slab
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(in_slab_window(a));
    kfree(a);

    void* b = kmalloc(kSlabMaxObj + 1);  // 2049 -> large
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(in_direct_map(b));
    kfree(b);
}

}  // namespace test_kmalloc_boundary

// ============================================================
// Test 3: small round-trip + zeroing
// ============================================================

namespace test_kmalloc_small {

void test_small_roundtrip() {
    void* p = kmalloc(100);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_zero(p, 100));
    fill(p, 100, 0x5A);
    TEST_ASSERT_TRUE(check(p, 100, 0x5A));
    kfree(p);
}

}  // namespace test_kmalloc_small

// ============================================================
// Test 4: large round-trip + zeroing + no stale leak on re-alloc
// ============================================================

namespace test_kmalloc_large {

void test_large_roundtrip() {
    void* p = kmalloc(4096);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_zero(p, 4096));
    fill(p, 4096, 0xC3);
    TEST_ASSERT_TRUE(check(p, 4096, 0xC3));
    kfree(p);

    void* q = kmalloc(4096);  // fresh allocation must be zeroed
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_TRUE(is_zero(q, 4096));
    kfree(q);
}

}  // namespace test_kmalloc_large

// ============================================================
// Test 5: very large multi-page allocation
// ============================================================

namespace test_kmalloc_huge {

void test_multipage() {
    constexpr size_t kSz = 65536;  // 16 pages
    void*           p   = kmalloc(kSz);
    TEST_ASSERT_NOT_NULL(p);
    fill(p, kSz, 0x77);
    TEST_ASSERT_TRUE(check(p, kSz, 0x77));
    kfree(p);
}

}  // namespace test_kmalloc_huge

// ============================================================
// Test 6: large alloc/free moves the free-page count symmetrically
// ============================================================

namespace test_kmalloc_accounting {

void test_free_page_count() {
    uint64_t before = g_pmm.free_page_count();
    void*    p      = kmalloc(8192);  // 2 pages
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_LT(g_pmm.free_page_count(), before);
    kfree(p);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), before);
}

}  // namespace test_kmalloc_accounting

// ============================================================
// Test 7: alignment -- small >= 16, large page-aligned
// ============================================================

namespace test_kmalloc_align {

void test_alignment() {
    void* s = kmalloc(13);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ(reinterpret_cast<uintptr_t>(s) % 16, 0ULL);
    kfree(s);

    void* l = kmalloc(5000);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQ(reinterpret_cast<uintptr_t>(l) % 4096, 0ULL);
    kfree(l);
}

}  // namespace test_kmalloc_align

// ============================================================
// Test 8: operator new / delete integration (scalar + array)
// ============================================================

namespace test_kmalloc_new {

void test_operator_new() {
    auto* i = new int(42);
    TEST_ASSERT_NOT_NULL(i);
    TEST_ASSERT_EQ(*i, 42);
    delete i;

    auto* arr = new char[300];  // -> slab 512-class
    TEST_ASSERT_NOT_NULL(arr);
    arr[0]   = 'X';
    arr[299] = 'Y';
    TEST_ASSERT_EQ(arr[0], 'X');
    TEST_ASSERT_EQ(arr[299], 'Y');
    delete[] arr;
}

}  // namespace test_kmalloc_new

// ============================================================
// Test 9: aligned new takes the large / page path
// ============================================================

namespace test_kmalloc_aligned_new {

void test_aligned_new() {
    void* p = operator new(64, std::align_val_t{4096});
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 4096, 0ULL);
    operator delete(p, std::align_val_t{4096});
}

}  // namespace test_kmalloc_aligned_new

// ============================================================
// Test 10: nullptr / out-of-window free is a no-op
// ============================================================

namespace test_kmalloc_null {

void test_null_invalid_free() {
    uint8_t stack_byte = 0;
    kfree(nullptr);
    kfree(&stack_byte);  // not in any window -> ignored
    TEST_ASSERT_TRUE(true);
}

}  // namespace test_kmalloc_null

// ============================================================
// Test 11: churn -- interleaved small + large, no corruption
// ============================================================

namespace test_kmalloc_churn {

void test_churn() {
    void* smalls[64];
    for (int i = 0; i < 64; i++) {
        smalls[i] = kmalloc(static_cast<size_t>(16 + i * 16));
        TEST_ASSERT_NOT_NULL(smalls[i]);
        fill(smalls[i], 16, static_cast<uint8_t>(i));
    }

    void* big = kmalloc(4096 * 4);
    TEST_ASSERT_NOT_NULL(big);
    fill(big, 4096 * 4, 0xEE);

    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQ(static_cast<uint8_t*>(smalls[i])[0], static_cast<uint8_t>(i));
        kfree(smalls[i]);
    }

    TEST_ASSERT_TRUE(check(big, 4096 * 4, 0xEE));  // big untouched by small churn
    kfree(big);
}

}  // namespace test_kmalloc_churn

// ============================================================
// Entry point
// ============================================================

extern "C" void run_kmalloc_tests() {
    TEST_SECTION("kmalloc/kfree Tests (F2-M7b)");

    RUN_TEST(test_kmalloc_routing::test_routing);
    RUN_TEST(test_kmalloc_boundary::test_boundary);
    RUN_TEST(test_kmalloc_small::test_small_roundtrip);
    RUN_TEST(test_kmalloc_large::test_large_roundtrip);
    RUN_TEST(test_kmalloc_huge::test_multipage);
    RUN_TEST(test_kmalloc_accounting::test_free_page_count);
    RUN_TEST(test_kmalloc_align::test_alignment);
    RUN_TEST(test_kmalloc_new::test_operator_new);
    RUN_TEST(test_kmalloc_aligned_new::test_aligned_new);
    RUN_TEST(test_kmalloc_null::test_null_invalid_free);
    RUN_TEST(test_kmalloc_churn::test_churn);

    TEST_SUMMARY();
}
