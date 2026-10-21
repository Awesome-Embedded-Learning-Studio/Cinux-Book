/**
 * @file    irq_stubs.hpp
 * @brief   The IDT's second half: sixteen gates for device interrupts.
 *
 * Exceptions arm vectors 0..31 in one batch at bring-up; this family
 * arms 0x20..0x2F the same way, one compiler-generated stub per vector
 * from the x86 interrupt attribute. Every stub runs the neutral
 * interrupt table and then acknowledges the chip — drivers never do
 * either. Installing the gates also seats the platform's timer line:
 * vector 0x20 being the heartbeat is x86 wiring knowledge, so it lives
 * here in the arch layer, in the no-SSE island.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::arch::irq {

/**
 * @brief         Arms all sixteen IRQ gates and seats the heartbeat.
 *
 * @return        None
 * @note          Safe to call once the table is live (lidt already
 *                done): gates take effect as they are written. The
 *                timer's seat in the dispatch table is filled here,
 *                before any gate can fire.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void InstallIrqStubs();

/**
 * @brief Switch the interrupt service to the firmware APIC topology.
 * @return True when the local and IO controller are ready.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
[[nodiscard]] bool InstallApicBackend();

/**
 * @brief Arm the local timer and reschedule interrupt gates.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
void InstallLocalInterrupts();

}  // namespace cinux::arch::irq
