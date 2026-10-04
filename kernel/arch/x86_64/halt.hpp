/**
 * @file    halt.hpp
 * @brief   The kernel's parking instruction.
 *
 * One hlt in a loop with interrupts as they stand: the final stop of
 * every clean end and every early failure. Lives in arch because the
 * instruction belongs to the CPU, not to any service above it.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/**
 * @brief         Parks the CPU forever with interrupts as they stand.
 *
 * @return        None
 * @note          The final stop of every early failure and every clean
 *                end; callers that need silence park their own
 *                interrupts first.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
[[noreturn]] inline void Halt() {
    for (;;) {
        asm volatile("hlt");
    }
}

}  // namespace cinux::arch
