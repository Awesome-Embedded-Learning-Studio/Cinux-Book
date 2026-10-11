/**
 * @file    rtc.hpp
 * @brief   The CMOS real-time clock as the wall-clock source.
 *
 * A singleton whose chip holds every bit of state: the kernel asks for
 * the current second and the driver deals with the 1984 hardware —
 * binary-or-BCD bytes, 12-or-24-hour days, a century register that
 * some boards never got. Everything below the single read face lives
 * in the .cpp. The reading is a Unix-second count with UTC semantics;
 * turning calendar fields into that count is base::time's job, not the
 * chip's.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/singleton.hpp"
#include "cinux/time.hpp"

namespace cinux::driver {

/**
 * @brief   One raw register read from the clock chip.
 * @note    The seven time-and-date bytes as they came off the ports,
 *          before any format question is answered. Public because the
 *          decode is pure arithmetic and host tests drive it directly.
 * @since   0.2.0
 * @ingroup kernel_driver
 */
struct RawClock {
    uint8_t seconds;  ///< Register 0x00.
    uint8_t minutes;  ///< Register 0x02.
    uint8_t hours;    ///< Register 0x04, PM bit rides bit 7 in 12-hour mode.
    uint8_t day;      ///< Register 0x07.
    uint8_t month;    ///< Register 0x08.
    uint8_t year;     ///< Register 0x09, two digits.
    uint8_t century;  ///< Register 0x32, two digits, absent on some boards.
};

/**
 * @brief         Status register B's format bits, as the chip answers.
 * @param[in]     status_b   The raw register B byte.
 * @return        True when the time bytes are binary (not BCD).
 * @since         0.2.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] constexpr bool ClockIsBinary(uint8_t status_b) {
    return base::bit::BitMask<uint8_t>{status_b}.has(base::bit::MaskBit<uint8_t>(2));
}

/**
 * @brief         Status register B's hour-format bit.
 * @param[in]     status_b   The raw register B byte.
 * @return        True when hours run 0..23, false for 12-hour plus PM bit.
 * @since         0.2.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] constexpr bool ClockIsTwentyFourHour(uint8_t status_b) {
    return base::bit::BitMask<uint8_t>{status_b}.has(base::bit::MaskBit<uint8_t>(1));
}

/**
 * @brief         Turns one raw reading into Unix seconds.
 *
 * @param[in]     raw        The register bytes as read.
 * @param[in]     status_b   Register B, carrying the format bits.
 * @return        Seconds since 1970-01-01T00:00:00Z, UTC semantics.
 * @note          Pure arithmetic over the caller's bytes — BCD or
 *                binary, 12- or 24-hour, midnight-is-twelve and all —
 *                so host tests drive all four format quadrants. A
 *                century byte outside 19..30 falls back to guessing
 *                from the release year, which stays right to 2099.
 * @warning       The fields are trusted; sanity is the caller's read
 *                discipline (read twice, require agreement).
 * @since         0.2.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] constexpr unsigned long long DecodeClock(const RawClock& raw, uint8_t status_b) {
    const bool kBinary = ClockIsBinary(status_b);
    const bool kCivil  = ClockIsTwentyFourHour(status_b);

    const auto kFromBcd = [](uint8_t bcd) {
        const auto kLow  = static_cast<uint8_t>(bcd & base::bit::Ones<uint8_t>(4));
        const auto kHigh = static_cast<uint8_t>(bcd >> 4U);
        return static_cast<uint8_t>(kLow + (kHigh * 10U));
    };

    const unsigned long kSeconds = kBinary ? raw.seconds : kFromBcd(raw.seconds);
    const unsigned long kMinutes = kBinary ? raw.minutes : kFromBcd(raw.minutes);
    const unsigned long kDay     = kBinary ? raw.day : kFromBcd(raw.day);
    const unsigned long kMonth   = kBinary ? raw.month : kFromBcd(raw.month);
    const unsigned long kYearTwo = kBinary ? raw.year : kFromBcd(raw.year);

    constexpr unsigned long kReleaseYear = 2026;
    const bool              kAfternoon =
        base::bit::BitMask<uint8_t>{raw.hours}.has(base::bit::MaskBit<uint8_t>(7));
    const auto    kHourBits = static_cast<uint8_t>(raw.hours & base::bit::Ones<uint8_t>(7));
    unsigned long hours     = kBinary ? kHourBits : kFromBcd(kHourBits);
    if (!kCivil) {
        if (kAfternoon && hours != 12UL) {
            hours += 12UL;
        } else if (!kAfternoon && hours == 12UL) {
            hours = 0UL;
        }
    }

    unsigned long century = 0;
    if (raw.century != 0) {
        century = kBinary ? raw.century : kFromBcd(raw.century);
    }
    if (century < 19 || century > 30) {
        century = kReleaseYear / 100UL;
        if ((century * 100UL) + kYearTwo < kReleaseYear) {
            century += 1UL;
        }
    }

    return base::time::UnixSecondsOf({.year   = (century * 100UL) + kYearTwo,
                                      .month  = kMonth,
                                      .day    = kDay,
                                      .hour   = hours,
                                      .minute = kMinutes,
                                      .second = kSeconds});
}

/**
 * @brief   Real-time clock behind CMOS ports 0x70/0x71.
 * @note    Meyers-singleton shape after Pit; the chip keeps the time,
 *          the instance carries nothing and the zero-construction rule
 *          holds. No init face: the clock is already running when the
 *          kernel gets control, and this driver never writes a CMOS
 *          byte (writes collide with BIOS checksums).
 * @since   0.1.0
 * @ingroup kernel_driver
 */
class Rtc : public cinux::base::Singleton<Rtc> {
    friend class cinux::base::Singleton<Rtc>;

public:
    /**
     * @brief         Reads the wall clock once.
     *
     * @return        Seconds since 1970-01-01T00:00:00Z, UTC semantics.
     * @note          Uses the read-twice-agree method against the
     *                once-a-second register update, so the answer never
     *                mixes two ticks. Busy-polls the chip briefly; the
     *                teaching kernel asks at most a few times per boot
     *                (file timestamps), never per-tick.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long long now_seconds();

private:
    Rtc() = default;
};

}  // namespace cinux::driver
