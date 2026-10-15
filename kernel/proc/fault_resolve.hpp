/**
 * @file    fault_resolve.hpp
 * @brief   Wires the page-fault resolver seat to the two repair stories.
 *
 * The arch reporter owns the seat and knows nothing about memory
 * policy; the memory theme owns both repairs and knows nothing about
 * scheduling. CoW answers the write-against-shared slice, demand
 * filling answers the not-present slice, and both want the running
 * task's world. This seat is the only place that has seen all three
 * sides: it offers each fault to CoW first — the older, narrower
 * story — and hands what declines to the demand filler over the
 * current task's space and register. Install it once at bring-up,
 * next to the diagnoser it accompanies.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::proc {

/**
 * @brief         Seats the current-task fault resolver into the reporter.
 *
 * @return        None.
 * @note          After this runs, every page fault is offered to CoW
 *                and then to demand filling before it dies. Safe before
 *                the first task exists: with no current task the seat
 *                declines and the fault takes the old road to the dump.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void InstallFaultResolver();

}  // namespace cinux::proc
