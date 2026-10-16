/**
 * @file    panic.hpp
 * @brief   The kernel's last words, spoken in one place.
 *
 * Every death — a violated Check, an unresolved exception, anything the
 * machine cannot survive — ends at this face: a frame chain walked with
 * names attached, the task that was running, the memory the machine has
 * left, and then silence. Callers print their own reason line first (a
 * Check knows its file and line, an exception knows its vector and
 * registers; those facts live where they are cheap) and hand this face
 * the frame to start the walk from, because only the caller knows whose
 * stack the story starts on.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_debug
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::debug {

/**
 * @brief         Print the panic aftermath and park the machine.
 * @param[in]     frame_base   rbp of the frame the walk starts from.
 * @return        None. Does not return.
 * @note          Three sections follow the caller's reason line: the
 *                backtrace (named when a symbol table is installed,
 *                raw addresses otherwise), the current task (or the
 *                admission that kernel Main is all that runs), and the
 *                memory overview — physical free and managed, heap free
 *                and top, the user world's recorded regions. Every part
 *                must survive a machine that may have died of memory
 *                exhaustion, so the walk allocates nothing.
 * @since         0.1.0
 * @ingroup       kernel_debug
 */
[[noreturn]] void PanicDump(unsigned long long frame_base);

}  // namespace cinux::debug
