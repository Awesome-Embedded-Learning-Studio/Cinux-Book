/**
 * @file    trace_config.hpp
 * @brief   Tunable facts of the stack tracer.
 *
 * The one knob of the tracer: how many frames a single walk may print
 * before it stops. The cap exists for the crashed case, not the healthy
 * one — a corrupted frame chain can loop or wander into unmapped memory,
 * and a dump that faults while dumping helps nobody.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_debug
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::debug {

/// Frames one walk prints at most before giving up.
inline constexpr unsigned int kMaxTraceFrames = 16;

}  // namespace cinux::debug
