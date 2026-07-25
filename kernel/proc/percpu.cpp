/**
 * @file kernel/proc/percpu.cpp
 * @brief Per-CPU control blocks (F4-M3 Phase 1).
 *
 * P1-2: `percpu()` reads `MSR_GS_BASE`, which the swapgs discipline keeps
 * pointed at this CPU's control block throughout kernel mode.  The scheduler
 * initialises `percpu_blocks[0]` (BSP) in `usermode_init`.
 */

#include "kernel/proc/percpu.hpp"

#include "kernel/arch/x86_64/msr.hpp"

namespace cinux::proc {

alignas(4096) PerCpu percpu_blocks[kMaxCpus];

PerCpu* percpu() {
    // MSR_GS_BASE == &percpu_blocks[cpu] in kernel mode (swapgs discipline).
    return reinterpret_cast<PerCpu*>(cinux::arch::read_msr(cinux::arch::kMsrGsBase));
}

void update_syscall_stack(uint64_t stack_top) {
    // %gs:0 reads PerCpu.kernel_stack directly (GS_BASE -> this block), so a
    // plain store here is exactly what syscall_entry will observe.
    percpu()->kernel_stack = stack_top;
}

}  // namespace cinux::proc
