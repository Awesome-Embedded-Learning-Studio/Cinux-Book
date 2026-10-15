#include <stdint.h>

#include "../framework/framework.hpp"
#include "cinux/time.hpp"
#include "kernel/driver/rtc/rtc.hpp"
#include "test_assert.hpp"

using cinux::driver::ClockIsBinary;
using cinux::driver::ClockIsTwentyFourHour;
using cinux::driver::DecodeClock;
using cinux::driver::RawClock;

constexpr uint8_t kStatusBcd24h    = 0x02;
constexpr uint8_t kStatusBinary24h = 0x06;
constexpr uint8_t kStatusBcd12h    = 0x00;
constexpr uint8_t kStatusBinary12h = 0x04;

TEST("rtc: status b answers the format questions") {
    ASSERT_TRUE(ClockIsBinary(kStatusBinary24h));
    ASSERT_TRUE(!ClockIsBinary(kStatusBcd24h));
    ASSERT_TRUE(ClockIsTwentyFourHour(kStatusBcd24h));
    ASSERT_TRUE(!ClockIsTwentyFourHour(kStatusBcd12h));
}

TEST("rtc: bcd and binary agree at the same afternoon") {
    const RawClock kBcd{
        .seconds = 0x05,
        .minutes = 0x01,
        .hours   = 0x13,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x20,
    };
    const RawClock kBinary{
        .seconds = 5,
        .minutes = 1,
        .hours   = 13,
        .day     = 11,
        .month   = 10,
        .year    = 26,
        .century = 20,
    };
    const unsigned long long kWant = cinux::base::time::UnixSecondsOf(
        {.year = 2026, .month = 10, .day = 11, .hour = 13, .minute = 1, .second = 5});
    ASSERT_TRUE(DecodeClock(kBcd, kStatusBcd24h) == kWant);
    ASSERT_TRUE(DecodeClock(kBinary, kStatusBinary24h) == kWant);
}

TEST("rtc: twelve-hour noon is twelve in both formats") {
    const RawClock kBcdNoon{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x92,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x20,
    };
    const RawClock kBinNoon{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x8C,
        .day     = 11,
        .month   = 10,
        .year    = 26,
        .century = 20,
    };
    const unsigned long long kWant = cinux::base::time::UnixSecondsOf(
        {.year = 2026, .month = 10, .day = 11, .hour = 12, .minute = 0, .second = 0});
    ASSERT_TRUE(DecodeClock(kBcdNoon, kStatusBcd12h) == kWant);
    ASSERT_TRUE(DecodeClock(kBinNoon, kStatusBinary12h) == kWant);
}

TEST("rtc: twelve-hour midnight is zero in both formats") {
    const RawClock kBcdMidnight{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x12,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x20,
    };
    const RawClock kBinMidnight{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x0C,
        .day     = 11,
        .month   = 10,
        .year    = 26,
        .century = 20,
    };
    const unsigned long long kWant = cinux::base::time::UnixSecondsOf(
        {.year = 2026, .month = 10, .day = 11, .hour = 0, .minute = 0, .second = 0});
    ASSERT_TRUE(DecodeClock(kBcdMidnight, kStatusBcd12h) == kWant);
    ASSERT_TRUE(DecodeClock(kBinMidnight, kStatusBinary12h) == kWant);
}

TEST("rtc: one in the afternoon is thirteen in both formats") {
    const RawClock kBcdAfternoon{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x81,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x20,
    };
    const RawClock kBinAfternoon{
        .seconds = 0,
        .minutes = 0,
        .hours   = 0x81,
        .day     = 11,
        .month   = 10,
        .year    = 26,
        .century = 20,
    };
    const unsigned long long kWant = cinux::base::time::UnixSecondsOf(
        {.year = 2026, .month = 10, .day = 11, .hour = 13, .minute = 0, .second = 0});
    ASSERT_TRUE(DecodeClock(kBcdAfternoon, kStatusBcd12h) == kWant);
    ASSERT_TRUE(DecodeClock(kBinAfternoon, kStatusBinary12h) == kWant);
}

TEST("rtc: a missing or wild century falls back to the release era") {
    const RawClock kNoCentury{
        .seconds = 0x05,
        .minutes = 0x01,
        .hours   = 0x13,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x00,
    };
    const RawClock kWildCentury{
        .seconds = 0x05,
        .minutes = 0x01,
        .hours   = 0x13,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0xFF,
    };
    const RawClock kSaneCentury{
        .seconds = 0x05,
        .minutes = 0x01,
        .hours   = 0x13,
        .day     = 0x11,
        .month   = 0x10,
        .year    = 0x26,
        .century = 0x20,
    };
    const unsigned long long kWant = cinux::base::time::UnixSecondsOf({
        .year   = 2026,
        .month  = 10,
        .day    = 11,
        .hour   = 13,
        .minute = 1,
        .second = 5,
    });
    ASSERT_TRUE(DecodeClock(kNoCentury, kStatusBcd24h) == kWant);
    ASSERT_TRUE(DecodeClock(kWildCentury, kStatusBcd24h) == kWant);
    ASSERT_TRUE(DecodeClock(kSaneCentury, kStatusBcd24h) == kWant);
}

int main() {
    return cinux::test::RunAll();
}
