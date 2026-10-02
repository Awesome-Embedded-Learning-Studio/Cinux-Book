/**
 * @file    math.hpp
 * @brief   Unit-aligned arithmetic: round down, round up, count the span.
 *
 * Small and OS-independent on purpose — the unit is a parameter, so pages,
 * sectors, or any other granularity use the same three verbs.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_math
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::math {

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

}  // namespace cinux::base::math
