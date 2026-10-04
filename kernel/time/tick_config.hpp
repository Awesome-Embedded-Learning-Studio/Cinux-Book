/**
 * @file    tick_config.hpp
 * @brief   Tunable facts of the tick service.
 *
 * The one knob of the heartbeat: how many interrupts per second the
 * service asks its backend for. Lives in its own file after the
 * pmm_config.hpp precedent, so tuning the system means touching a config
 * file, never a class face. Device facts (the PIT input clock, ports,
 * divisor arithmetic) are not service knobs and stay inside pit.cpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_time
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"

namespace cinux::time {

using cinux::base::operator""_Hz;

/// Heartbeat rate the kernel asks its tick backend for.
inline constexpr cinux::base::Hertz kTickHz = 100_Hz;

}  // namespace cinux::time
