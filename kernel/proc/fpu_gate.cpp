#include "kernel/proc/fpu_gate.hpp"

#include "cinux/assert.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

extern "C" void EnterFpuGate() {
    const cinux::arch::IrqGuard kGuard;
    auto* const                 kCurrent = cinux::proc::Scheduler::self().current();
    if (kCurrent == nullptr) {
        return;
    }
    if (kCurrent->fpu_gate_depth++ == 0) {
        cinux::arch::SaveFpu(kCurrent->entry_fpu);
    }
}

extern "C" void ExitFpuGate() {
    const cinux::arch::IrqGuard kGuard;
    auto* const                 kCurrent = cinux::proc::Scheduler::self().current();
    if (kCurrent == nullptr) {
        return;
    }
    cinux::base::safety::Check(kCurrent->fpu_gate_depth != 0, "unbalanced FPU gate exit");
    if (--kCurrent->fpu_gate_depth == 0) {
        cinux::arch::RestoreFpu(kCurrent->entry_fpu);
    }
}
