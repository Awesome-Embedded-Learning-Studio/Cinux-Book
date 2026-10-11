/**
 * @file    time.hpp
 * @brief   Time words: calendar arithmetic and durations.
 *
 * Two families live here. The calendar family turns six naive
 * wall-clock fields — year, month, day, hour, minute, second — into
 * the one number file systems and logs want: seconds since
 * 1970-01-01T00:00:00Z (Howard Hinnant's days_from_civil shift,
 * correct for every date the hardware can hold). The duration family
 * carries spans of time as strong types with literals after the Hertz
 * precedent — 500_ms is a Milliseconds, never a bare number the
 * reader must decode — and every span converts through one common
 * gate, ToNanoseconds.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_time
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::time {

/**
 * @brief   A wall-clock reading split into calendar fields.
 * @note    Zero-padded values as they come off a clock chip; the range
 *          is whatever the producer guarantees, the arithmetic here
 *          trusts it.
 * @since   0.1.0
 * @ingroup base_time
 */
struct CivilTime {
    unsigned long year;    ///< Full year, e.g. 2026.
    unsigned long month;   ///< 1..12, January is 1.
    unsigned long day;     ///< 1..31.
    unsigned long hour;    ///< 0..23.
    unsigned long minute;  ///< 0..59.
    unsigned long second;  ///< 0..59 (leap seconds read as 59).
};

/**
 * @brief         Seconds between the epoch and a civil reading.
 *
 * @param[in]     clock   The calendar reading, UTC semantics.
 * @return        Seconds since 1970-01-01T00:00:00Z.
 * @note          Pure arithmetic: months shift March..February so the
 *                leap day lands last in the year, then the era shift
 *                (146097 days per 400 years) counts days before the
 *                remaining months and days are folded in.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_time
 */
constexpr unsigned long long UnixSecondsOf(const CivilTime& clock) {
    const unsigned long kMarchYear  = clock.month <= 2UL ? clock.year - 1UL : clock.year;
    const unsigned long kEra        = kMarchYear / 400UL;
    const unsigned long kYearInEra  = kMarchYear - (kEra * 400UL);
    const unsigned long kMonthShift = (clock.month + 9UL) % 12UL;
    const unsigned long kDayOfYear  = (((153UL * kMonthShift) + 2UL) / 5UL) + (clock.day - 1UL);
    const unsigned long kDayOfEra =
        (kYearInEra * 365UL) + (kYearInEra / 4UL) - (kYearInEra / 100UL) + kDayOfYear;
    const unsigned long long kDays = (static_cast<unsigned long long>(kEra) * 146097ULL) +
                                     static_cast<unsigned long long>(kDayOfEra) - 719468ULL;
    return (kDays * 86400ULL) + (static_cast<unsigned long long>(clock.hour) * 3600ULL) +
           (static_cast<unsigned long long>(clock.minute) * 60ULL) + clock.second;
}

/**
 * @brief   A span of time in whole seconds.
 * @note    Strong type after Hertz: a duration crosses interfaces as
 *          its own type so no reader decodes a unit-less number.
 * @since   0.2.0
 * @ingroup base_time
 */
struct Seconds {
    unsigned long long value;  ///< Whole seconds.
};

/// @brief A span of time in whole milliseconds. @sa Seconds
struct Milliseconds {
    unsigned long long value;  ///< Whole milliseconds.
};

/// @brief A span of time in whole microseconds. @sa Seconds
struct Microseconds {
    unsigned long long value;  ///< Whole microseconds.
};

/// @brief A span of time in whole nanoseconds. @sa Seconds
struct Nanoseconds {
    unsigned long long value;  ///< Whole nanoseconds.
};

/** @brief         Literal in seconds. */
constexpr Seconds operator""_s(unsigned long long value) {
    return Seconds{.value = value};
}

/** @brief         Literal in milliseconds. */
constexpr Milliseconds operator""_ms(unsigned long long value) {
    return Milliseconds{.value = value};
}

/** @brief         Literal in microseconds. */
constexpr Microseconds operator""_us(unsigned long long value) {
    return Microseconds{.value = value};
}

/** @brief         Literal in nanoseconds. */
constexpr Nanoseconds operator""_ns(unsigned long long value) {
    return Nanoseconds{.value = value};
}

/**
 * @brief         The common gate every span converts through.
 * @param[in]     span   The span to widen.
 * @return        The same span counted in nanoseconds.
 * @note          Comparing or mixing durations happens here, in one
 *                place, after the Hertz pattern of one spelled-out
 *                multiply per unit.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_time
 */
constexpr Nanoseconds ToNanoseconds(Seconds span) {
    return Nanoseconds{.value = span.value * 1000000000ULL};
}

/// @brief The common gate, milliseconds in. @return The span in
/// nanoseconds. @sa ToNanoseconds(Seconds)
constexpr Nanoseconds ToNanoseconds(Milliseconds span) {
    return Nanoseconds{.value = span.value * 1000000ULL};
}

/// @brief The common gate, microseconds in. @return The span in
/// nanoseconds. @sa ToNanoseconds(Seconds)
constexpr Nanoseconds ToNanoseconds(Microseconds span) {
    return Nanoseconds{.value = span.value * 1000ULL};
}

/// @brief The common gate, nanoseconds in (identity). @return The same
/// span. @sa ToNanoseconds(Seconds)
constexpr Nanoseconds ToNanoseconds(Nanoseconds span) {
    return span;
}

}  // namespace cinux::base::time
