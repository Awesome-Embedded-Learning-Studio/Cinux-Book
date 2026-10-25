/**
 * @file    timer_config.hpp
 * @brief   Tunable facts of the timer table.
 *
 * The one capacity knob of the timer service: how many armed seats
 * the table keeps at once. Lives in its own file after the
 * tick_config.hpp precedent, so tuning the system means touching a
 * config file, never a class face.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_time
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::time {

/**
 * @brief   Armed seats the timer table keeps at once.
 * @since   0.7.0
 * @ingroup kernel_time
 */
inline constexpr unsigned long kTimerSeats = 16;

/**
 * @brief   Deadline the table judges against, in monotonic nanoseconds.
 * @since   0.7.0
 * @ingroup kernel_time
 */
using TimerDeadline = unsigned long long;

}  // namespace cinux::time
