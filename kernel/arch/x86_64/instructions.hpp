/**
 * @file    instructions.hpp
 * @brief   Little execution-hint primitives the kernel spells as asm.
 *
 * One-line instructions that mean something to the CPU but nothing to
 * C++ gather here under plain names, so call sites read intent instead
 * of AT&T syntax. Today: the spin-wait hint. The interrupt save and
 * restore pair lives one file over in irq_guard.hpp; heavier machine
 * work (context switch) stays in its own assembly file.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch {

/**
 * @brief         The spin-wait hint: tell the CPU this loop is waiting.
 * @return        None.
 * @note          Pauses the pipeline a beat, saves power, and lets a
 *                hyperthread sibling make progress — the polite thing
 *                to do inside a spin loop. No memory semantics.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
inline void CpuRelax() {
    __asm__ volatile("pause" : : : "memory");
}

}  // namespace cinux::arch
