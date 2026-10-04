/**
 * @file    irq_config.hpp
 * @brief   Capacity facts of the interrupt service.
 *
 * How many device lines one dispatch table seats. A platform-shape
 * number after the pmm_config.hpp precedent: capacities live in config
 * files, never in announce headers.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_interrupt
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::interrupt {

/// Seats in the dispatch table: one chip pair, sixteen lines.
inline constexpr unsigned int kIrqLineCount = 16;

}  // namespace cinux::interrupt
