/**
 * @file    fault_diagnose.hpp
 * @brief   Wires the page-fault diagnoser seat to the running task's register.
 *
 * The arch reporter owns the seat and knows nothing about tasks; the
 * memory theme owns the register and knows nothing about scheduling.
 * This one function is the only place that has seen both sides: it
 * answers the seat's call by naming the running task and handing the
 * fault to its range register. Install it once at bring-up, next to the
 * resolver it accompanies.
 *
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::proc {

/**
 * @brief         Seats the current-task fault diagnoser into the reporter.
 *
 * @return        None.
 * @note          After this runs, every page fault the resolver declines
 *                is judged once against the running task's range register
 *                before the dump. Safe before the first task exists: an
 *                empty seat prints the no-register line and returns.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void InstallFaultDiagnoser();

}  // namespace cinux::proc
