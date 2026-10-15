/**
 * @file    math.hpp
 * @brief   Small arithmetic verbs: pick, bound, round, count the span.
 *
 * Small and OS-independent on purpose — the unit is a parameter, so pages,
 * sectors, or any other granularity use the same verbs.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_math
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::math {

/**
 * @brief         Picks the smaller of two values.
 *
 * @param[in]     left   One value.
 * @param[in]     right  The other value.
 * @return        The minimum.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_math
 */
constexpr unsigned long Min(unsigned long left, unsigned long right) {
    return left < right ? left : right;
}

/**
 * @brief         Picks the larger of two values.
 *
 * @param[in]     left   One value.
 * @param[in]     right  The other value.
 * @return        The maximum.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_math
 */
constexpr unsigned long Max(unsigned long left, unsigned long right) {
    return left > right ? left : right;
}

/**
 * @brief         Bounds a value into a closed interval.
 *
 * @param[in]     value   The value to bound.
 * @param[in]     floor   The lowest allowed value.
 * @param[in]     top     The highest allowed value.
 * @return        value, or the nearest bound it crossed.
 * @note          An inverted interval (floor above top) answers top.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_math
 */
constexpr unsigned long Clamp(unsigned long value, unsigned long floor, unsigned long top) {
    return Min(Max(value, floor), top);
}

/**
 * @brief         Rounds a value down to a whole number of units.
 *
 * @param[in]     value   Quantity to floor.
 * @param[in]     unit    Unit size; must be nonzero.
 * @return        value / unit.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_math
 */
constexpr unsigned long Floor(unsigned long value, unsigned long unit) {
    return value / unit;
}

/**
 * @brief         Rounds a value up to a whole number of units.
 *
 * @param[in]     value   Quantity to ceiling.
 * @param[in]     unit    Unit size; must be nonzero.
 * @return        value / unit rounded up.
 * @note          Callers staying far from ULONG_MAX keep this overflow-free.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_math
 */
constexpr unsigned long Ceil(unsigned long value, unsigned long unit) {
    return (value + unit - 1) / unit;
}

/**
 * @brief         Counts whole units covering the half-open interval.
 *
 * @param[in]     base   Interval start, inclusive.
 * @param[in]     top    Interval end, exclusive.
 * @param[in]     unit   Unit size; must be nonzero.
 * @return        Units from the floor of base to the ceiling of top; empty
 *                intervals report zero.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_math
 */
constexpr unsigned long Span(unsigned long base, unsigned long top, unsigned long unit) {
    return top > base ? Ceil(top, unit) - Floor(base, unit) : 0;
}

/**
 * @brief         Rounds a byte count up to a multiple of a unit.
 *
 * @param[in]     value   Byte count; may already be aligned.
 * @param[in]     unit    Alignment unit; must be nonzero.
 * @return        The smallest multiple of unit that is at least value.
 * @note          Ceil counts how many units cover the value; this one hands
 *                back the covered byte count, for any unit.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_math
 */
constexpr unsigned long AlignUp(unsigned long value, unsigned long unit) {
    return Ceil(value, unit) * unit;
}

/**
 * @brief         Rounds a byte count down to a multiple of a unit.
 *
 * @param[in]     value   Byte count; may already be aligned.
 * @param[in]     unit    Alignment unit; must be nonzero.
 * @return        The largest multiple of unit that is at most value.
 * @note          The downward sibling of AlignUp: an address lands on the
 *                page holding it, not the one after.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_math
 */
constexpr unsigned long AlignDown(unsigned long value, unsigned long unit) {
    return Floor(value, unit) * unit;
}

}  // namespace cinux::base::math
