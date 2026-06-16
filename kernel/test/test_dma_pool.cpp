/**
 * @file kernel/test/test_dma_pool.cpp
 * @brief QEMU in-kernel tests for DmaPool (M3-2)
 *
 * Exercises the DMA allocator end-to-end through the real PMM/VMM: allocation
 * yields a mapped, CPU-accessible buffer paired via the direct-map window;
 * free (explicit and RAII) returns pages and unmaps; OOM and the size==0 guard
 * are reported as errors.  Requires PMM + VMM initialised.
 */

#include <stdint.h>

#include <utility>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::arch::KERNEL_VMA;
using cinux::drivers::dma::g_dma_pool;
using cinux::lib::Error;
using cinux::mm::g_pmm;
using cinux::mm::g_vmm;

// ============================================================
// Test 1: alloc produces a valid, mapped buffer
// ============================================================

namespace test_dpool_alloc {

void test_alloc_mapped() {
    auto r = g_dma_pool.alloc(4096);  // one page
    TEST_ASSERT_TRUE(r.ok());
    auto buf = std::move(r.value());
    TEST_ASSERT_TRUE(buf.valid());
    TEST_ASSERT_TRUE(buf.size() == 4096);
    TEST_ASSERT_TRUE(buf.phys() != 0);
    // virt must be phys in the higher-half direct-map window
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(buf.virt()), buf.phys() + KERNEL_VMA);
    // mapping is live: translate agrees with phys
    TEST_ASSERT_EQ(g_vmm.translate(reinterpret_cast<uint64_t>(buf.virt())), buf.phys());
}

}  // namespace test_dpool_alloc

// ============================================================
// Test 2: CPU can read/write the mapped memory
// ============================================================

namespace test_dpool_roundtrip {

void test_cpu_access() {
    auto r = g_dma_pool.alloc(4096);
    TEST_ASSERT_TRUE(r.ok());
    auto              buf = std::move(r.value());
    volatile uint8_t* p   = reinterpret_cast<volatile uint8_t*>(buf.virt());
    p[0]    = 0xAB;
    p[4095] = 0xCD;
    TEST_ASSERT_EQ(p[0], 0xAB);
    TEST_ASSERT_EQ(p[4095], 0xCD);
}

}  // namespace test_dpool_roundtrip

// ============================================================
// Test 3: explicit free returns the data pages
// ============================================================
// Note: free_page_count is not expected to return exactly to `before`.  The
// direct map is demand-paged, so alloc()'s map() walk may allocate shared
// page-table pages that are never reclaimed; we assert only the direction
// (alloc consumes, free returns the data pages).

namespace test_dpool_free_count {

void test_free_restores_pages() {
    const uint64_t before = g_pmm.free_page_count();
    uint64_t       mid    = 0;
    {
        auto r = g_dma_pool.alloc(8192);  // 2 data pages
        TEST_ASSERT_TRUE(r.ok());
        auto buf = std::move(r.value());
        mid = g_pmm.free_page_count();
        TEST_ASSERT_TRUE(mid < before);  // alloc consumed pages
        g_dma_pool.free(buf);
        TEST_ASSERT_FALSE(buf.valid());
    }
    TEST_ASSERT_TRUE(g_pmm.free_page_count() > mid);  // data pages returned
}

}  // namespace test_dpool_free_count

// ============================================================
// Test 4: RAII destructor returns pages automatically
// ============================================================

namespace test_dpool_raii {

void test_destructor_releases() {
    uint64_t mid = 0;
    {
        auto r = g_dma_pool.alloc(4096);
        TEST_ASSERT_TRUE(r.ok());
        auto buf = std::move(r.value());
        mid = g_pmm.free_page_count();
    }  // ~DmaBuffer fires the release hook -> free_pages
    // Data page returned (direct-map PTE left in place by design).
    TEST_ASSERT_TRUE(g_pmm.free_page_count() > mid);
}

}  // namespace test_dpool_raii

// ============================================================
// Test 5: OOM is reported as Error::OutOfMemory
// ============================================================

namespace test_dpool_oom {

void test_oom_returns_error() {
    const uint64_t avail = g_pmm.free_page_count();
    auto           r     = g_dma_pool.alloc((avail + 1) * 4096);  // one page over budget
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(r.error(), Error::OutOfMemory);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), avail);  // nothing leaked
}

}  // namespace test_dpool_oom

// ============================================================
// Test 6: size == 0 is rejected
// ============================================================

namespace test_dpool_zero {

void test_zero_size_rejected() {
    auto r = g_dma_pool.alloc(0);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(r.error(), Error::InvalidArgument);
}

}  // namespace test_dpool_zero

// ============================================================
// Entry point
// ============================================================

extern "C" void run_dma_pool_tests() {
    TEST_SECTION("DmaPool Tests (M3-2)");

    RUN_TEST(test_dpool_alloc::test_alloc_mapped);
    RUN_TEST(test_dpool_roundtrip::test_cpu_access);
    RUN_TEST(test_dpool_free_count::test_free_restores_pages);
    RUN_TEST(test_dpool_raii::test_destructor_releases);
    RUN_TEST(test_dpool_oom::test_oom_returns_error);
    RUN_TEST(test_dpool_zero::test_zero_size_rejected);

    TEST_SUMMARY();
}
