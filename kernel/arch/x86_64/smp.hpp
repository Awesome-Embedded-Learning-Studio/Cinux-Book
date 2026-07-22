/**
 * @file kernel/arch/x86_64/smp.hpp
 * @brief AP (Application Processor) boot interface (F4-M3 Phase 2)
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

/// Reschedule IPI vector (F4-M4 M4-2).  Sent to idle APs to pull them out of
/// `hlt` so they re-check the shared run queue.  Chosen to avoid the PIC IRQ
/// range (0x20-0x2F), the spurious vector (0xFF) and the sigreturn trap (0x80).
constexpr uint8_t kRescheduleIpiVector = 0xE0;

/// LAPIC timer vector (F5-M5 -smp).  Each AP's local APIC timer fires here to
/// drive Scheduler::tick() (the BSP uses the PIT for the same role; the PIT
/// reaches the BSP only).  0x30 avoids the PIC range and xHCI (0x40).
constexpr uint8_t kLapicTimerVector = 0x30;

/// Wake an idle AP (if any) so it picks up a newly runnable task from the
/// shared run queue.  Sends a reschedule IPI to every online AP; redundant
/// IPIs are harmless (an AP that finds the queue empty just halts again), so
/// this needs no precise per-CPU idle tracking.  No-op on a single-core system
/// (no APs are online -> nothing to wake).
void wake_idle_ap();

/// Boot every Application Processor listed in the ACPI MADT (BSP-side).
///
/// Run once during init, after the scheduler and the APIC are up.  Each AP is
/// driven through the INIT-SIPI-SIPI sequence, reaches 64-bit long mode via the
/// trampoline at physical 0x8000, runs ap_main(), and then idles (it does not
/// run user tasks -- that is M4 multi-core scheduling).  No-op when there is
/// only one CPU.
void boot_aps();

}  // namespace cinux::arch
