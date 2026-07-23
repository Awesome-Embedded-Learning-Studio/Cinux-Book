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
#include "kernel/arch/x86_64/memory_layout.hpp"  // DIRECT_MAP_BASE
#include "kernel/arch/x86_64/paging.hpp"         // PageEntry
#include "kernel/arch/x86_64/paging_config.hpp"  // FLAG_*
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/process_internal.hpp"  // copy_page_table_level

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

// F-VERIFY M5-1: drive the REAL CoW-marking path (copy_page_table_level) over a
// REAL AddressSpace.  The test above (test_simulated_fork_cow_lifecycle) calls
// mapcount_inc/dec BY HAND; this one exercises fork.cpp's actual page-table
// walk + CoW PTE marking + mapcount bump, with no scheduler needed (the function
// takes src/dst PHYS, not a task).  Replaces the 0x1234-sentinel / bare-struct
// fake coverage the M1 matrix flagged as the #1 fake test.
void test_real_copy_page_table_level_cow() {
    using namespace cinux::arch;
    // 1. Parent AS; map a writable user page V -> P; store a marker at P.
    cinux::mm::AddressSpace parent;
    uint64_t                p = g_pmm.alloc_page();
    TEST_ASSERT_NE(p, 0ull);
    auto* marker         = reinterpret_cast<volatile uint64_t*>(DIRECT_MAP_BASE + p);
    *marker              = 0xDEADBEEF12345678ULL;
    constexpr uint64_t V = 0x40000000ULL;  // a user vaddr
    TEST_ASSERT_TRUE(parent.map(V, p, FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER));
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 1);

    // 2. Alloc + zero a child PML4, then run the REAL CoW clone over it.
    uint64_t child_pml4 = g_pmm.alloc_page();
    TEST_ASSERT_NE(child_pml4, 0ull);
    auto* ct = reinterpret_cast<volatile PageEntry*>(DIRECT_MAP_BASE + child_pml4);
    for (int j = 0; j < 512; j++) {
        ct[j].raw = 0;
    }
    // Run the REAL CoW clone.  Level semantics: level 1 = PT (leaf), so
    // 4=PML4(root)->3 PDPT->2 PD->1 PT(leaf).  fork() uses an OUTER PML4 loop +
    // level 3 per PDPT; calling with the PML4 root + level 4 is equivalent (the
    // function iterates PML4 internally at level>1) and clones the whole tree.
    cinux::proc::copy_page_table_level(parent.pml4_phys(), child_pml4, 4);

    // 3. The real CoW path bumped mapcount to 2 (shared parent+child) -- NOT a
    //    hand-simulated inc.  This is the F10 DEBT-003 fix's domain.
    TEST_ASSERT_EQ(g_pmm.mapcount_load(p), 2);

    // 4. Child shares the same physical page (translate via the child PML4).
    TEST_ASSERT_EQ(cinux::mm::g_vmm.translate(V, &child_pml4), p);

    // 5. Parent's mapping is unchanged (still resolves to P).
    TEST_ASSERT_EQ(parent.translate(V), p);

    // Cleanup.  Intermediate PT pages copy_page_table_level allocated leak in
    // test scope -- acceptable for a one-shot in-kernel test (QEMU exits after).
    g_pmm.mapcount_dec_and_test(p);
    g_pmm.mapcount_dec_and_test(p);
    g_pmm.free_page(p);
    g_pmm.free_page(child_pml4);
}

}  // namespace test_pmm_mapcount

extern "C" void run_pmm_mapcount_tests() {
    TEST_SECTION("PMM mapcount (F-QA Q4b)");
    RUN_TEST(test_pmm_mapcount::test_alloc_sets_mapcount_one);
    RUN_TEST(test_pmm_mapcount::test_inc_bumps_mapcount);
    RUN_TEST(test_pmm_mapcount::test_dec_and_test_false_above_zero);
    RUN_TEST(test_pmm_mapcount::test_dec_and_test_true_at_zero);
    RUN_TEST(test_pmm_mapcount::test_simulated_fork_cow_lifecycle);
    RUN_TEST(test_pmm_mapcount::test_real_copy_page_table_level_cow);  // F-VERIFY M5-1
    TEST_SUMMARY();
}
