#include "kernel/proc/fault_diagnose.hpp"

#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/mm/vma_diag.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

namespace {

void diagnose_current_fault(unsigned long long address, unsigned long long error_code) {
    const Task* const kSelf = Scheduler::self().current();
    if (kSelf == nullptr || kSelf->vmas == nullptr) {
        print::Println("[kern]  vma: addr=%X outside any task world", address);
        return;
    }
    mm::PrintFaultVerdict(kSelf->vmas, address, error_code);
}

}  // namespace

void InstallFaultDiagnoser() {
    arch::isr::InstallPageFaultDiagnoser(&diagnose_current_fault);
}

}  // namespace cinux::proc
