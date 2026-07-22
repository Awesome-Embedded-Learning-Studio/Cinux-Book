/**
 * @file kernel/test/test_aslr.cpp
 * @brief ASLR offset helper tests (F9 batch 8 / M2)
 *
 * Verifies the three offset helpers (stack / mmap / brk) return page-aligned
 * values inside their declared ranges, and that consecutive draws actually
 * vary -- i.e. the helpers feed a real PRNG stream, not a constant. g_random
 * self-inits on first use (random.cpp); the harness has PIT/TSC up by here.
 *
 * What this does NOT prove: that end-to-end addresses differ across runs
 * (run-kernel-test is a single boot). That is checked by eye via the
 * "[PROC] jumping to user mode: ... stack_top=%p" line under `make run`.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/lib/aslr.hpp"

using cinux::lib::aslr_brk_offset;
using cinux::lib::aslr_mmap_offset;
using cinux::lib::aslr_stack_offset;

namespace {

constexpr uint64_t kPageMask = 0xFFFULL;

// Upper bounds on each offset (mask + one page). See aslr.hpp for the masks.
constexpr uint64_t kStackMax = 0x800000ULL;    // mask 0x7FF000  -> 0 .. 8 MiB
constexpr uint64_t kMmapMax  = 0x40000000ULL;  // mask 0x3FFFF000 -> 0 .. 1 GiB
constexpr uint64_t kBrkMax   = 0x1000000ULL;   // mask 0xFFF000  -> 0 .. 16 MiB

bool all_same(const uint64_t* vals, int n) {
    for (int i = 1; i < n; ++i) {
        if (vals[i] != vals[0]) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace test_aslr {

void test_aslr_stack_offset_range() {
    for (int i = 0; i < 64; ++i) {
        const uint64_t v = aslr_stack_offset();
        TEST_ASSERT_TRUE((v & kPageMask) == 0);  // page-aligned
        TEST_ASSERT_LE(v, kStackMax);
    }
}

void test_aslr_mmap_offset_range() {
    for (int i = 0; i < 64; ++i) {
        const uint64_t v = aslr_mmap_offset();
        TEST_ASSERT_TRUE((v & kPageMask) == 0);
        TEST_ASSERT_LE(v, kMmapMax);
    }
}

void test_aslr_brk_offset_range() {
    for (int i = 0; i < 64; ++i) {
        const uint64_t v = aslr_brk_offset();
        TEST_ASSERT_TRUE((v & kPageMask) == 0);
        TEST_ASSERT_LE(v, kBrkMax);
    }
}

void test_aslr_offsets_vary() {
    // 16 draws of each must not collapse to a single value. Each call advances
    // the xoshiro256** state, so even a fixed seed yields a varying stream; a
    // constant result would mean the helper is broken (e.g. ignoring the PRNG).
    uint64_t s[16], m[16], b[16];
    for (int i = 0; i < 16; ++i) {
        s[i] = aslr_stack_offset();
        m[i] = aslr_mmap_offset();
        b[i] = aslr_brk_offset();
    }
    TEST_ASSERT_FALSE(all_same(s, 16));
    TEST_ASSERT_FALSE(all_same(m, 16));
    TEST_ASSERT_FALSE(all_same(b, 16));
}

}  // namespace test_aslr

// ============================================================
// Entry point
// ============================================================

extern "C" void run_aslr_tests() {
    TEST_SECTION("ASLR Offset Tests (F9 batch 8)");

    RUN_TEST(test_aslr::test_aslr_stack_offset_range);
    RUN_TEST(test_aslr::test_aslr_mmap_offset_range);
    RUN_TEST(test_aslr::test_aslr_brk_offset_range);
    RUN_TEST(test_aslr::test_aslr_offsets_vary);

    TEST_SUMMARY();
}
