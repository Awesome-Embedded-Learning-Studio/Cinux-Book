#include "../framework/framework.hpp"
#include "cinux/time.hpp"
#include "test_assert.hpp"

using cinux::base::time::CivilTime;
using cinux::base::time::UnixSecondsOf;

using cinux::base::time::operator""_s;
using cinux::base::time::operator""_ms;
using cinux::base::time::operator""_us;
using cinux::base::time::operator""_ns;

TEST("time: the epoch itself is zero") {
    const CivilTime kEpoch{.year = 1970, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 0};
    ASSERT_TRUE(UnixSecondsOf(kEpoch) == 0ULL);
}

TEST("time: a leap day lands") {
    const CivilTime kLeap{.year = 2000, .month = 2, .day = 29, .hour = 0, .minute = 0, .second = 0};
    ASSERT_TRUE(UnixSecondsOf(kLeap) == 951782400ULL);
}

TEST("time: an october morning in 2026") {
    const CivilTime kFall{
        .year   = 2026,
        .month  = 10,
        .day    = 6,
        .hour   = 5,
        .minute = 20,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kFall) == 1791264000ULL);
}

TEST("time: year boundaries cross both ways") {
    const CivilTime kNewYearEve{
        .year   = 1999,
        .month  = 12,
        .day    = 31,
        .hour   = 23,
        .minute = 59,
        .second = 59,
    };
    ASSERT_TRUE(UnixSecondsOf(kNewYearEve) == 946684799ULL);
    const CivilTime kNewYear{
        .year   = 2000,
        .month  = 1,
        .day    = 1,
        .hour   = 0,
        .minute = 0,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kNewYear) == 946684800ULL);
}

TEST("time: the non-leap century year skips february 29") {
    const CivilTime kMarchFirst{
        .year   = 1900,
        .month  = 3,
        .day    = 1,
        .hour   = 0,
        .minute = 0,
        .second = 0,
    };
    const CivilTime kFebruaryTwentyEight{
        .year   = 1900,
        .month  = 2,
        .day    = 28,
        .hour   = 0,
        .minute = 0,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kMarchFirst) - UnixSecondsOf(kFebruaryTwentyEight) == 86400ULL);
}

TEST("time: the year 2100 lands where the epoch math says") {
    const CivilTime kFebruaryTwentyEight{
        .year   = 2100,
        .month  = 2,
        .day    = 28,
        .hour   = 0,
        .minute = 0,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kFebruaryTwentyEight) == 4107456000ULL);
    const CivilTime kMarchFirst{
        .year   = 2100,
        .month  = 3,
        .day    = 1,
        .hour   = 0,
        .minute = 0,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kMarchFirst) == 4107542400ULL);
}

TEST("time: the year 2400 keeps its leap day") {
    const CivilTime kLeapNoon{
        .year   = 2400,
        .month  = 2,
        .day    = 29,
        .hour   = 12,
        .minute = 0,
        .second = 0,
    };
    ASSERT_TRUE(UnixSecondsOf(kLeapNoon) == 13574606400ULL);
}

TEST("time: duration literals carry their unit") {
    ASSERT_TRUE((2_s).value == 2ULL);
    ASSERT_TRUE((500_ms).value == 500ULL);
    ASSERT_TRUE((3_us).value == 3ULL);
    ASSERT_TRUE((7_ns).value == 7ULL);
}

TEST("time: every span meets at the nanosecond gate") {
    const auto kHalfSecond = cinux::base::time::ToNanoseconds(500_ms);
    ASSERT_TRUE(kHalfSecond.value == 500000000ULL);
    ASSERT_TRUE(cinux::base::time::ToNanoseconds(2_s).value == 2000000000ULL);
    ASSERT_TRUE(cinux::base::time::ToNanoseconds(4_us).value == 4000ULL);
    ASSERT_TRUE(cinux::base::time::ToNanoseconds(9_ns).value == 9ULL);
}

TEST("time: one second in milliseconds and seconds agree at the gate") {
    const auto kAsSeconds = cinux::base::time::ToNanoseconds(1_s);
    const auto kAsMillis  = cinux::base::time::ToNanoseconds(1000_ms);
    const auto kAsMicros  = cinux::base::time::ToNanoseconds(1000000_us);
    const auto kAsNanos   = cinux::base::time::ToNanoseconds(1000000000_ns);
    ASSERT_TRUE(kAsSeconds.value == kAsMillis.value);
    ASSERT_TRUE(kAsMillis.value == kAsMicros.value);
    ASSERT_TRUE(kAsMicros.value == kAsNanos.value);
}

int main() {
    return cinux::test::RunAll();
}

TEST("time: deadline arithmetic saturates before duration multiplication or addition") {
    using cinux::base::time::DeadlineAfter;
    ASSERT_EQ(DeadlineAfter(10_ns, 2_s, 3_ns).value, 2000000013ULL);
    ASSERT_EQ(DeadlineAfter(cinux::base::time::Nanoseconds{~0ULL - 4}, 0_s, 4_ns).value, ~0ULL);
    ASSERT_EQ(DeadlineAfter(cinux::base::time::Nanoseconds{~0ULL - 4}, 0_s, 5_ns).value, ~0ULL);
    ASSERT_EQ(DeadlineAfter(0_ns, cinux::base::time::Seconds{~0ULL}, 0_ns).value, ~0ULL);
}
