/**
 * @file kernel/test/test_pmm_pte_count.cpp
 * @brief QEMU in-kernel tests for the split pte_count / refcount PMM model
 *
 * Batch 3 split: pte_count = user-PTE mapping count (never frees), refcount =
 * ownership refs (alloc baseline + cache/shm owners; last ref frees).  These
 * tests cover the new contract: alloc sets refcount=1/pte_count=0, the pure
 * pte_count inc/dec, the refcount API, the bundled pte_count_dec_and_test
 * (drops a mapping ref and, on the last PTE, the ownership ref that frees),
 * the page-cache "survives teardown" guarantee, and the fork-CoW lifecycle.
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

namespace test_pmm_pte_count {

void test_alloc_sets_refcount_one_pte_count_zero() {
    uint64_t p = g_pmm.alloc_page();
    TEST_ASSERT_NE(p, 0ull);
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 0);  // no PTE maps it yet
    TEST_ASSERT_EQ(g_pmm.refcount_load(p), 1);   // alloc ownership baseline
    g_pmm.refcount_dec_and_test(p);              // last ref -> frees
}

void test_pte_count_inc_dec_pure() {
    uint64_t p = g_pmm.alloc_page();  // pte_count=0, refcount=1
    g_pmm.pte_count_inc(p);           // 1
    g_pmm.pte_count_inc(p);           // 2
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 2);
    g_pmm.pte_count_dec(p);  // 1 -- pure, never frees
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 1);
    g_pmm.pte_count_dec(p);  // 0
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 0);
    g_pmm.refcount_dec_and_test(p);  // drop ownership -> frees
}

void test_refcount_dec_and_test_frees_on_last() {
    uint64_t p = g_pmm.alloc_page();  // refcount=1
    g_pmm.refcount_inc(p);            // 2
    TEST_ASSERT_FALSE(g_pmm.refcount_dec_and_test(p));  // 2->1, not last
    TEST_ASSERT_EQ(g_pmm.refcount_load(p), 1);
    TEST_ASSERT_TRUE(g_pmm.refcount_dec_and_test(p));  // 1->0 -> frees
}

void test_dec_and_test_bundles_refcount() {
    // User data page lifecycle (anon): install bumps pte_count; teardown
    // (pte_count_dec_and_test) reaches 0 on the mapping counter and drops the
    // alloc-baseline ownership ref, freeing the page on the last ref.
    uint64_t p = g_pmm.alloc_page();  // pte_count=0, refcount=1
    g_pmm.pte_count_inc(p);           // install: pte_count=1
    TEST_ASSERT_TRUE(g_pmm.pte_count_dec_and_test(p));  // pte_count 1->0, refcount 1->0 -> freed
}

void test_cache_page_survives_teardown() {
    // The lto_plugin-corruption guarantee: a page-cache page holds an extra
    // ownership ref (CachePhysRef) on top of the alloc baseline, so teardown
    // -- which only drops the mapping ref -- leaves refcount > 0 and the page
    // alive in the cache.
    uint64_t p = g_pmm.alloc_page();  // refcount=1
    g_pmm.refcount_inc(p);            // cache own (CachePhysRef): refcount=2
    g_pmm.pte_count_inc(p);           // install: pte_count=1
    // teardown: pte_count reaches 0, refcount 2->1 -> NOT freed (cache owns)
    TEST_ASSERT_FALSE(g_pmm.pte_count_dec_and_test(p));
    TEST_ASSERT_EQ(g_pmm.refcount_load(p), 1);  // cache own alive
    TEST_ASSERT_TRUE(g_pmm.refcount_dec_and_test(p));  // cache evict: 1->0 -> frees
}

void test_simulated_fork_cow_lifecycle() {
    // fork CoW share: parent install (pte_count_inc), fork CoW share (another
    // pte_count_inc), child teardown drops one mapping WITHOUT freeing (parent
    // still maps), parent teardown drops the last mapping and frees.
    uint64_t p = g_pmm.alloc_page();  // pte_count=0, refcount=1
    g_pmm.pte_count_inc(p);           // parent install: 1
    g_pmm.pte_count_inc(p);           // fork CoW share: 2
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 2);

    bool child_freed = g_pmm.pte_count_dec_and_test(p);  // child exec: 2->1 (NOT last)
    TEST_ASSERT_FALSE(child_freed);                      // parent still maps -> must NOT free
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 1);

    bool parent_freed = g_pmm.pte_count_dec_and_test(p);  // parent exit: 1->0 -> refcount 1->0
    TEST_ASSERT_TRUE(parent_freed);                       // last ref gone -> freed
}

// F-VERIFY M5-1: drive the REAL CoW-marking path (copy_page_table_level) over a
// REAL AddressSpace.  The hand-simulated tests above call pte_count_inc/dec
// directly; this one exercises fork.cpp's actual page-table walk + CoW PTE
// marking + pte_count bump, with no scheduler needed.
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
    g_pmm.pte_count_inc(p);  // batch 3: caller accounts for the installed PTE
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 1);

    // 2. Alloc + zero a child PML4, then run the REAL CoW clone over it.
    uint64_t child_pml4 = g_pmm.alloc_page();
    TEST_ASSERT_NE(child_pml4, 0ull);
    auto* ct = reinterpret_cast<volatile PageEntry*>(DIRECT_MAP_BASE + child_pml4);
    for (int j = 0; j < 512; j++) {
        ct[j].raw = 0;
    }
    cinux::proc::copy_page_table_level(parent.pml4_phys(), child_pml4, 4);

    // 3. The real CoW path bumped pte_count to 2 (parent install + fork share).
    TEST_ASSERT_EQ(g_pmm.pte_count_load(p), 2);

    // 4. Child shares the same physical page (translate via the child PML4).
    TEST_ASSERT_EQ(cinux::mm::g_vmm.translate(V, &child_pml4), p);

    // 5. Parent's mapping is unchanged (still resolves to P).
    TEST_ASSERT_EQ(parent.translate(V), p);

    // Cleanup.  Two teardown refs (parent + child) drop the page; intermediate
    // PT pages copy_page_table_level allocated leak in test scope -- acceptable
    // for a one-shot in-kernel test (QEMU exits after).
    g_pmm.pte_count_dec_and_test(p);  // child teardown (or parent)
    g_pmm.pte_count_dec_and_test(p);  // last -> frees
    g_pmm.free_page(child_pml4);
}

}  // namespace test_pmm_pte_count

extern "C" void run_pmm_pte_count_tests() {
    TEST_SECTION("PMM pte_count/refcount (C refactor batch 3)");
    RUN_TEST(test_pmm_pte_count::test_alloc_sets_refcount_one_pte_count_zero);
    RUN_TEST(test_pmm_pte_count::test_pte_count_inc_dec_pure);
    RUN_TEST(test_pmm_pte_count::test_refcount_dec_and_test_frees_on_last);
    RUN_TEST(test_pmm_pte_count::test_dec_and_test_bundles_refcount);
    RUN_TEST(test_pmm_pte_count::test_cache_page_survives_teardown);
    RUN_TEST(test_pmm_pte_count::test_simulated_fork_cow_lifecycle);
    RUN_TEST(test_pmm_pte_count::test_real_copy_page_table_level_cow);  // F-VERIFY M5-1
    TEST_SUMMARY();
}
