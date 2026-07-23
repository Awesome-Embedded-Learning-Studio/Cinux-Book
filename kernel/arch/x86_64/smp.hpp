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

// ============================================================
// F-VERIFY M3-2: AP test-mode self-check hook
// ============================================================
// When g_ap_test_selfcheck_fn != nullptr, each AP -- right after signalling
// online (g_aps_online++), BEFORE entering the scheduler spin -- calls it,
// stores its readback into g_ap_selfcheck_results[cpu_id], and halts forever
// (cli;hlt) instead of entering the scheduler idle path (the test kernel runs
// no scheduler).  Defaults to nullptr so PRODUCTION BEHAVIOR IS UNCHANGED (the
// branch is skipped).  The test kernel sets this + does LAPIC-IPI init +
// boot_aps to turn run-kernel-test-smp from a BSP-only no-op into a real AP-wake
// + AP-side mechanism-readback gate (cracks the 47/47 SMP-空转 blind spot AND
// delivers the AP column of the mechanism-readback matrix -- the LSTAR==0 #DF
// class).  See M3-2 in PLAN「🔄 F-VERIFY」.
struct ApSelfcheckResult {
    uint32_t cpu_id;
    uint32_t magic;   // kApSelfcheckMagic once the AP actually ran the fn
    uint64_t cr4;     // expect OSFXSR(9)|OSXMMEXCPT(10)|PAE(5); SMEP(20)|SMAP(21) if exposed
    uint64_t efer;    // IA32_EFER (0xC0000080): NXE(11) | SCE(0) | LME(8)
    uint64_t lstar;   // IA32_LSTAR (0xC0000082): syscall RIP -- MUST be non-zero
    uint64_t star;    // IA32_STAR  (0xC0000081)
    uint64_t sfmask;  // IA32_FMASK (0xC0000084)
};
constexpr uint32_t kApSelfcheckMagic = 0xA5C0FFEE;

// Return: true = after the readback, enter the production scheduler spin+idle
// (so the AP can pick up cross-core work, e.g. forktest children for the
// F-VERIFY M5-2b cross-core CoW stress); false = halt forever (cli;hlt) -- used
// when the test kernel runs no scheduler (suite-only -smp gate).  The test
// kernel decides based on its own config (smoke on/off), keeping this production
// header free of test-flag coupling.
using ApSelfcheckFn = bool (*)(uint32_t cpu_id);

// Defined in ap_main.cpp.  Results sized [proc::kMaxCpus] at the definition
// (ap_main.cpp pulls in percpu.hpp); declared unsized here to avoid that dep.
extern ApSelfcheckResult g_ap_selfcheck_results[];
extern ApSelfcheckFn     g_ap_test_selfcheck_fn;  // default nullptr (ap_main.cpp)

}  // namespace cinux::arch
