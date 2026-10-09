/**
 * @file    irq_guard.hpp
 * @brief   The interrupt-flag discipline as a ScopedGuard policy.
 *
 * Entering reads rflags into the state slot and clears IF; exiting
 * restores what was read. That memory is the whole point: a nested
 * guard opened with interrupts off must leave them off when it
 * closes, so the exit is a conditional restore, never an unconditional
 * sti — the classic early-enable bug cannot be written in this shape.
 * The machine instructions live behind SaveAndDisableIrq/RestoreIrq,
 * one declaration with two definitions selected at link time: the
 * kernel's real pushfq/cli pair, and a host no-op that lets lock and
 * scheduler logic run their full state machines off-target.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/scoped_guard.hpp"

namespace cinux::arch {

/**
 * @brief         Snapshot the interrupt state and disable interrupts.
 * @return        The rflags snapshot to hand back to RestoreIrq.
 * @note          Link-time selected: the kernel pushes and pops real
 *                flags; a host test world returns a stub so guard
 *                shapes and lock logic stay testable off-target.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
unsigned long long SaveAndDisableIrq();

/**
 * @brief         Put the interrupt state back as the snapshot found it.
 * @param[in]     snapshot   What SaveAndDisableIrq handed out.
 * @return        None.
 * @note          Interrupts return only if they were on at the
 *                matching save; the no-op host definition keeps the
 *                contract by doing nothing at all.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
void RestoreIrq(unsigned long long snapshot);

/**
 * @brief         Save-and-clear-IF policy for the ScopedGuard shell.
 * @note          State carries the rflags snapshot taken on enter.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
struct IrqLockPolicy {
    using State = unsigned long long;

    /**
     * @brief     Snapshot rflags into the slot, then clear IF.
     * @since     0.1.0
     * @ingroup   kernel_arch
     */
    static void enter(State& state) { state = SaveAndDisableIrq(); }

    /**
     * @brief     Restore IF only if the snapshot carried it set.
     * @since     0.1.0
     * @ingroup   kernel_arch
     */
    static void exit(State& state) { RestoreIrq(state); }
};

/// Interrupts saved on entry, restored on scope exit; nests safely.
using IrqGuard = cinux::base::ScopedGuard<IrqLockPolicy>;

/**
 * @brief         Enables interrupts and halts, as one adjacent pair.
 * @note          Contract: the caller runs at CPL0 with IF clear; the
 *                STI interrupt shadow delays recognition by one
 *                instruction, so the HLT executes before any handler —
 *                the sleep is atomic with the enable. Unrelated
 *                interrupts may wake the caller, who must therefore
 *                re-check the awaited condition in a loop. Never call
 *                this holding a spinlock, and never replace it with
 *                RestoreIrq plus a separate HLT: that pair reopens the
 *                wake-lost window this primitive exists to close.
 *                Host worlds get a yield, keeping the shape runnable.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void EnableIrqAndHalt();

}  // namespace cinux::arch
