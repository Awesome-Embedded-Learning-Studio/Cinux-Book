/**
 * @file kernel/test/test_memory_stats.cpp
 * @brief QEMU in-kernel tests for dump_memory_stats (FO batch 4)
 *
 * Verifies the summary runs without faulting after the MM subsystems are up
 * and that PMM reports a sane non-zero memory inventory.  The printed lines
 * are observed live on the serial output; the assertions guard the data.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/mm/diagnostics.hpp"
#include "kernel/mm/pmm.hpp"

using cinux::mm::dump_memory_stats;
using cinux::mm::g_pmm;

namespace test_memstats {

void test_dump_runs_and_reports() {
    dump_memory_stats();  // observed on serial; must not fault
    TEST_ASSERT_GT(g_pmm.total_page_count(), 0ULL);
    TEST_ASSERT_GT(g_pmm.free_page_count(), 0ULL);
}

}  // namespace test_memstats

extern "C" void run_memory_stats_tests() {
    TEST_SECTION("Memory Stats Tests (FO)");

    RUN_TEST(test_memstats::test_dump_runs_and_reports);

    TEST_SUMMARY();
}
