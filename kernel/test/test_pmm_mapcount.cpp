/**
 * @file kernel/test/test_pmm_mapcount.cpp
 * @brief QEMU in-kernel tests for PMM per-page mapcount (F-QA Q4b-1 / DEBT-003)
 *
 * Exercises the mapcount API that backs CoW page reference counting: alloc
 * sets 1, inc/dec are atomic, dec_and_test returns true only at the 0
 * transition, and a simulated fork-CoW lifecycle (share -> child exec drops
 * one ref without freeing -> parent exit frees) matches the DEBT-003 fix.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/mm/pmm.hpp"

using cinux::mm::g_pmm;

namespace test_pmm_mapcount {

void test_alloc_sets_mapcount_one() {
    uint64_t p = g_pmm.alloc_page();
    TEST_ASSERT_NE(p, 0ull);
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 1);
    // free does NOT touch mapcount (callers drive dec_and_test); the page just
    // returns to the buddy pool with a stale count, overwritten on next alloc.
    g_pmm.free_page(p);
}

void test_inc_bumps_mapcount() {
    uint64_t p = g_pmm.alloc_page();  // 1
    g_pmm.mapcount_inc(p);            // 2
    g_pmm.mapcount_inc(p);            // 3
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 3);
    g_pmm.mapcount_dec_and_test(p);        // 3->2
    g_pmm.mapcount_dec_and_test(p);        // 2->1
    if (g_pmm.mapcount_dec_and_test(p)) {  // 1->0
        g_pmm.free_page(p);
    }
}

void test_dec_and_test_false_above_zero() {
    uint64_t p = g_pmm.alloc_page();                    // 1
    g_pmm.mapcount_inc(p);                              // 2
    TEST_ASSERT_FALSE(g_pmm.mapcount_dec_and_test(p));  // 2->1, not last
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 1);
    if (g_pmm.mapcount_dec_and_test(p)) {  // 1->0
        g_pmm.free_page(p);
    }
}

void test_dec_and_test_true_at_zero() {
    uint64_t p = g_pmm.alloc_page();                   // 1
    TEST_ASSERT_TRUE(g_pmm.mapcount_dec_and_test(p));  // 1->0, last ref
    g_pmm.free_page(p);
}

void test_simulated_fork_cow_lifecycle() {
    // The DEBT-003 scenario: parent maps a page, fork CoW-shares it (inc),
    // child exec drops one ref WITHOUT freeing (parent still maps), parent
    // exit drops the last ref and frees.
    uint64_t p = g_pmm.alloc_page();  // parent: mapcount 1
    g_pmm.mapcount_inc(p);            // fork CoW share: 2 (parent+child)
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 2);

    bool child_freed = g_pmm.mapcount_dec_and_test(p);  // child exec: 2->1
    TEST_ASSERT_FALSE(child_freed);                     // parent still maps -> must NOT free
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 1);

    bool parent_freed = g_pmm.mapcount_dec_and_test(p);  // parent exit: 1->0
    TEST_ASSERT_TRUE(parent_freed);
    g_pmm.free_page(p);
}

}  // namespace test_pmm_mapcount

extern "C" void run_pmm_mapcount_tests() {
    TEST_SECTION("PMM mapcount (F-QA Q4b)");
    RUN_TEST(test_pmm_mapcount::test_alloc_sets_mapcount_one);
    RUN_TEST(test_pmm_mapcount::test_inc_bumps_mapcount);
    RUN_TEST(test_pmm_mapcount::test_dec_and_test_false_above_zero);
    RUN_TEST(test_pmm_mapcount::test_dec_and_test_true_at_zero);
    RUN_TEST(test_pmm_mapcount::test_simulated_fork_cow_lifecycle);
    TEST_SUMMARY();
}
