/**
 * @file kernel/test/test_rtc.cpp
 * @brief CMOS RTC wall-clock tests (F5-M4)
 *
 * Runs inside QEMU.  Verifies the RTC decodes a plausible wall-clock date and a
 * sane (post-2024) Unix epoch, which exercises the full port-I/O + BCD-decode +
 * UIP-guarded-read path against the live MC146818.  QEMU seeds the CMOS RTC from
 * the host clock, so the year reflects the real current date.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/rtc/rtc.hpp"

using cinux::drivers::DateTime;
using cinux::drivers::g_rtc;

// ============================================================
// RTC wall-clock readback (B3)
// ============================================================
namespace test_rtc_driver {

void test_read_sane_date() {
    g_rtc.init();
    const DateTime dt = g_rtc.read_datetime();
    // QEMU seeds the RTC from the host clock; expect the real current date.
    // Year window is generous so this does not need updating each year.
    TEST_ASSERT_TRUE(dt.year >= 2024 && dt.year <= 2099);
    TEST_ASSERT_TRUE(dt.month >= 1 && dt.month <= 12);
    TEST_ASSERT_TRUE(dt.day >= 1 && dt.day <= 31);
    TEST_ASSERT_TRUE(dt.hour < 24);
    TEST_ASSERT_TRUE(dt.minute < 60);
    TEST_ASSERT_TRUE(dt.second < 60);
}

void test_boot_epoch_sane() {
    g_rtc.init();
    TEST_ASSERT_TRUE(g_rtc.available());
    const int64_t epoch = g_rtc.boot_epoch_seconds();
    // After 2024-01-01 (1704067200) and before 2100-01-01 (4102444800).
    TEST_ASSERT_TRUE(epoch >= 1704067200LL && epoch < 4102444800LL);
}

}  // namespace test_rtc_driver

// ============================================================
// Entry point
// ============================================================
extern "C" void run_rtc_tests() {
    TEST_SECTION("RTC (F5-M4)");

    RUN_TEST(test_rtc_driver::test_read_sane_date);
    RUN_TEST(test_rtc_driver::test_boot_epoch_sane);

    TEST_SUMMARY();
}
