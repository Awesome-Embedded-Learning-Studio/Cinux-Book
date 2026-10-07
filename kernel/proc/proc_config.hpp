/**
 * @file    proc_config.hpp
 * @brief   Tunable facts of kernel threading.
 *
 * Three knobs, each a fact the rest of the proc theme reads: how many
 * pages one kernel-thread stack spans, the marker written at its bottom
 * as a post-mortem overflow probe, and how many tasks may sit ready at
 * once. Overflow of the ready queue is a Check failure, not a silent
 * drop — losing a wakeup is undebuggable, failing loudly is one serial
 * line.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/arch/x86_64/page.hpp"

namespace cinux::proc {

/// Pages of physical memory behind one kernel-thread stack.
inline constexpr unsigned int kStackPages = 4;

/// Marker at the very bottom of every thread stack; changed means the
/// stack ran past its end.
inline constexpr unsigned long long kStackMagic = 0xDEADC0DEULL;

/// Tasks that may wait in the ready queue at once.
inline constexpr unsigned int kReadyQueueCapacity = 16;

/// Timer ticks one task may run before the scheduler asks for a
/// rotation; two at 100 Hz is 20 ms per slice.
inline constexpr unsigned int kTimeSliceTicks = 2;

/// Bytes of one kernel-thread stack.
inline constexpr unsigned long kStackBytes =
    static_cast<unsigned long>(kStackPages) * cinux::arch::page::kSize;

}  // namespace cinux::proc
