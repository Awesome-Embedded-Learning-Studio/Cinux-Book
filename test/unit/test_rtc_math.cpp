/**
 * @file test/unit/test_rtc_math.cpp
 * @brief Unit tests for RTC pure helpers (F5-M4 B3).
 *
 * Pins the BCD decode and the Gregorian -> Unix-seconds conversion against known
 * epochs, independent of the port-I/O read path.  days_from_civil /
 * datetime_to_unix_seconds are the drift-correction arithmetic sys_clock_gettime
 * builds REALTIME on, so their exactness matters.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <stdint.h>

#include "kernel/drivers/rtc/rtc.hpp"
#include "test_framework.h"

using cinux::drivers::bcd_to_binary;
using cinux::drivers::datetime_to_unix_seconds;
using cinux::drivers::days_from_civil;

TEST("rtc_math: BCD decode") {
    ASSERT_EQ(bcd_to_binary(0x00), 0u);
    ASSERT_EQ(bcd_to_binary(0x09), 9u);
    ASSERT_EQ(bcd_to_binary(0x10), 10u);
    ASSERT_EQ(bcd_to_binary(0x26), 26u);
    ASSERT_EQ(bcd_to_binary(0x59), 59u);
    ASSERT_EQ(bcd_to_binary(0x99), 99u);
}

TEST("rtc_math: Unix epoch is day zero") {
    ASSERT_EQ(days_from_civil(1970, 1, 1), 0);
    ASSERT_EQ(datetime_to_unix_seconds(1970, 1, 1, 0, 0, 0), 0LL);
}

TEST("rtc_math: Y2K epoch") {
    // 2000-01-01 00:00:00 UTC = 946684800 (well-known).
    ASSERT_EQ(days_from_civil(2000, 1, 1), 10957);
    ASSERT_EQ(datetime_to_unix_seconds(2000, 1, 1, 0, 0, 0), 946684800LL);
}

TEST("rtc_math: 2024-01-01 epoch") {
    // 2024-01-01 00:00:00 UTC = 1704067200.
    ASSERT_EQ(datetime_to_unix_seconds(2024, 1, 1, 0, 0, 0), 1704067200LL);
}

TEST("rtc_math: time-of-day adds correctly") {
    // 2024-01-01 01:02:03 == 1704067200 + 1*3600 + 2*60 + 3.
    ASSERT_EQ(datetime_to_unix_seconds(2024, 1, 1, 1, 2, 3), 1704067200LL + 3723LL);
}

TEST("rtc_math: leap day counted") {
    // 2024 is a leap year; 2024-03-01 minus 2024-02-28 is 2 days (the 29th).
    const int64_t feb28 = days_from_civil(2024, 2, 28);
    const int64_t mar01 = days_from_civil(2024, 3, 1);
    ASSERT_EQ(mar01 - feb28, 2);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
