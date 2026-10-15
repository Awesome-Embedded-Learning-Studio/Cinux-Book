/**
 * @file    fpu_gate.hpp
 * @brief   Protects a task's floating-point state across kernel entries.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

extern "C" {

/**
 * @brief         Save the current task's state on its outermost entry.
 * @return        None.
 * @note          Nesting belongs to the task so blocking and preemption
 *                cannot transfer a pending gate to another task. Safe
 *                before a current task exists. Call from the no-SSE island.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void EnterFpuGate();

/**
 * @brief         Restore the current task's state on its outermost exit.
 * @return        None.
 * @note          Each returning entry must balance its own exit. A fork
 *                child starts outside the gate. Call from the no-SSE island.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void ExitFpuGate();
}
