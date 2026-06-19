/**
 * @file kernel/arch/x86_64/usermode.cpp
 * @brief User-mode (Ring 3) transition support implementation
 *
 * Provides usermode_init() which configures the STAR/EFER MSRs for SYSRET and
 * anchors this CPU's GS base at its PerCpu control block (F4-M3 P1-2).  Also
 * provides jump_to_usermode() (see usermode.S) for entering Ring 3.
 */

#include "kernel/arch/x86_64/usermode.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/percpu.hpp"

namespace cinux::arch {

extern "C" void usermode_init_asm();

// ============================================================
// Public interface
// ============================================================

void usermode_init() {
    usermode_init_asm();
    cinux::lib::kprintf("[USER] STAR/EFER MSRs configured for SYSRET.\n");

    // Anchor the BSP's GS base at its PerCpu block.  The swapgs discipline
    // (P1-2) keeps MSR_GS_BASE == &percpu_blocks[cpu] throughout kernel mode,
    // so syscall_entry reads %gs:0 == PerCpu.kernel_stack and percpu() reads
    // MSR_GS_BASE.  KERNEL_GS_BASE holds the user GS base (0) while in kernel.
    auto* bsp    = &cinux::proc::percpu_blocks[0];
    bsp->cpu_id  = 0;
    bsp->apic_id = 0;
    cinux::arch::write_msr(cinux::arch::kMsrGsBase, reinterpret_cast<uint64_t>(bsp));
    cinux::arch::write_msr(cinux::arch::kMsrKernelGsBase, 0);

    cinux::lib::kprintf("[USER] BSP PerCpu block at %p (GS base anchored)\n",
                        reinterpret_cast<void*>(bsp));
}

}  // namespace cinux::arch
