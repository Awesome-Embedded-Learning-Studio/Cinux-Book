/**
 * @file kernel/test/test_slab.cpp
 * @brief QEMU in-kernel tests for the SlabAllocator (F2-M7b batch 1)
 *
 * g_slab is initialised in main_test.cpp after g_vmm (slab maps pages via VMM
 * and draws them from the PMM).  Validates: alloc/free round-trip with zeroing,
 * size-class routing, slab fill -> grow, free -> LIFO reuse, cross-class mixing
 * without corruption, nullptr / out-of-window free no-ops, large-size
 * rejection, and default 16-byte alignment.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/mm/slab.hpp"

using cinux::mm::g_slab;
using cinux::mm::kSlabMaxObj;

namespace {

void fill(void* p, size_t n, uint8_t v) {
    auto* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
        b[i] = v;
    }
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
// Test 1: alloc/free round-trip + zeroing
// ============================================================

namespace test_slab_basic {

void test_alloc_free_basic() {
    void* p = g_slab.alloc(16);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(check(p, 16, 0));  // freshly allocated memory is zeroed
    fill(p, 16, 0xAB);
    TEST_ASSERT_TRUE(check(p, 16, 0xAB));
    g_slab.free(p);
}

}  // namespace test_slab_basic

// ============================================================
// Test 2: zero / oversized requests are rejected
// ============================================================

namespace test_slab_reject {

void test_reject_large_and_zero() {
    TEST_ASSERT_NULL(g_slab.alloc(0));
    TEST_ASSERT_NULL(g_slab.alloc(kSlabMaxObj + 1));  // not slab-eligible
}

}  // namespace test_slab_reject

// ============================================================
// Test 3: size-class routing -- one of each class, distinct pointers
// ============================================================

namespace test_slab_routing {

void test_size_routing() {
    void*  objs[8];
    size_t sizes[8] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    for (int i = 0; i < 8; i++) {
        objs[i] = g_slab.alloc(sizes[i]);
        TEST_ASSERT_NOT_NULL(objs[i]);
    }
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            TEST_ASSERT_NE(objs[i], objs[j]);
        }
    }
    for (int i = 0; i < 8; i++) {
        g_slab.free(objs[i]);
    }
}

}  // namespace test_slab_routing

// ============================================================
// Test 4: exhausting a slab forces a second slab page (growth)
// ============================================================

namespace test_slab_grow {

void test_fill_grows_slab() {
    constexpr int kN = 1024;
    void*         objs[kN];
    int           n = 0;
    for (int i = 0; i < kN; i++) {
        void* p = g_slab.alloc(16);
        if (p == nullptr) {
            break;
        }
        objs[n++] = p;
    }
    TEST_ASSERT_GT(n, 256);  // multiple slab pages were mapped
    for (int i = 0; i < n; i++) {
        g_slab.free(objs[i]);
    }
}

}  // namespace test_slab_grow

// ============================================================
// Test 5: free then alloc reuses the same slot (LIFO), no new page
// ============================================================

namespace test_slab_reuse {

void test_free_reuses_slot() {
    void* p1 = g_slab.alloc(48);  // -> 64-byte class
    TEST_ASSERT_NOT_NULL(p1);
    g_slab.free(p1);
    void* p2 = g_slab.alloc(48);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQ(p2, p1);  // freelist is LIFO
    g_slab.free(p2);
}

}  // namespace test_slab_reuse

// ============================================================
// Test 6: cross-class alloc/free interleave without corruption
// ============================================================

namespace test_slab_mixed {

void test_mixed_alloc_free() {
    void* a = g_slab.alloc(100);   // 128
    void* b = g_slab.alloc(200);   // 256
    void* c = g_slab.alloc(700);   // 1024
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    fill(a, 100, 0x11);
    fill(b, 200, 0x22);
    fill(c, 700, 0x33);

    g_slab.free(b);
    void* b2 = g_slab.alloc(200);
    TEST_ASSERT_NOT_NULL(b2);

    TEST_ASSERT_TRUE(check(a, 100, 0x11));  // untouched by b's recycle
    TEST_ASSERT_TRUE(check(c, 700, 0x33));

    g_slab.free(a);
    g_slab.free(b2);
    g_slab.free(c);
}

}  // namespace test_slab_mixed

// ============================================================
// Test 7: nullptr / out-of-window free is a no-op
// ============================================================

namespace test_slab_null {

void test_free_null_noop() {
    uint8_t stack_byte = 0;
    g_slab.free(nullptr);                                 // must not crash
    g_slab.free(&stack_byte);                             // outside slab window: ignored
    TEST_ASSERT_TRUE(true);
}

}  // namespace test_slab_null

// ============================================================
// Test 8: returned pointers are at least 16-byte aligned
// ============================================================

namespace test_slab_align {

void test_default_alignment() {
    const size_t sizes[5] = {16, 17, 100, 1000, 2048};
    for (int i = 0; i < 5; i++) {
        void*     p = g_slab.alloc(sizes[i]);
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQ(a % 16, 0ULL);
        g_slab.free(p);
    }
}

}  // namespace test_slab_align

// ============================================================
// Test 9: double-free is detected without corrupting state
// ============================================================

namespace test_slab_doublefree {

void test_double_free_detected() {
    void* p = g_slab.alloc(32);
    TEST_ASSERT_NOT_NULL(p);
    g_slab.free(p);
    g_slab.free(p);  // poison guard: detected and ignored

    // The allocator must remain usable afterwards.
    void* q = g_slab.alloc(32);
    TEST_ASSERT_NOT_NULL(q);
    fill(q, 32, 0x99);
    TEST_ASSERT_TRUE(check(q, 32, 0x99));
    g_slab.free(q);
}

}  // namespace test_slab_doublefree

// ============================================================
// Test 10: dedicated cache (arbitrary object size, e.g. 600 B)
// ============================================================

namespace test_slab_dedicated {

void test_dedicated_cache() {
    // A dedicated cache for an odd 600 B size avoids the 1024 B general-class
    // rounding.  (The cache struct itself is kmalloc'd and intentionally not
    // destroyed -- it is a test fixture.)
    auto* cache = g_slab.create_cache("test600", 600);
    TEST_ASSERT_NOT_NULL(cache);

    void* a = g_slab.cache_alloc(cache);
    void* b = g_slab.cache_alloc(cache);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NE(a, b);

    TEST_ASSERT_TRUE(check(a, 600, 0));   // freshly allocated -> zeroed
    fill(a, 600, 0x42);
    fill(b, 600, 0x84);
    TEST_ASSERT_TRUE(check(a, 600, 0x42));
    TEST_ASSERT_TRUE(check(b, 600, 0x84));

    g_slab.cache_free(cache, a);
    void* c = g_slab.cache_alloc(cache);  // LIFO reuse of a's slot
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQ(c, a);
    TEST_ASSERT_TRUE(check(c, 600, 0));   // re-alloc is zeroed (no stale leak)

    g_slab.cache_free(cache, b);
    g_slab.cache_free(cache, c);
}

}  // namespace test_slab_dedicated

// ============================================================
// Entry point
// ============================================================

extern "C" void run_slab_tests() {
    TEST_SECTION("Slab Allocator Tests (F2-M7b)");

    RUN_TEST(test_slab_basic::test_alloc_free_basic);
    RUN_TEST(test_slab_reject::test_reject_large_and_zero);
    RUN_TEST(test_slab_routing::test_size_routing);
    RUN_TEST(test_slab_grow::test_fill_grows_slab);
    RUN_TEST(test_slab_reuse::test_free_reuses_slot);
    RUN_TEST(test_slab_mixed::test_mixed_alloc_free);
    RUN_TEST(test_slab_null::test_free_null_noop);
    RUN_TEST(test_slab_align::test_default_alignment);
    RUN_TEST(test_slab_doublefree::test_double_free_detected);
    RUN_TEST(test_slab_dedicated::test_dedicated_cache);

    TEST_SUMMARY();
}
