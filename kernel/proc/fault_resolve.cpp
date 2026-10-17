/**
 * @file    fault_resolve.cpp
 * @brief   The seat's body: CoW first, then the register's promises.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#include "kernel/proc/fault_resolve.hpp"

#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/demand.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"

namespace cinux::proc {

namespace {

bool resolve_current_fault(unsigned long long address, unsigned long long error_code) {
    if (mm::cow::TryResolvePageFault(address, error_code)) {
        return true;
    }
    const Task* const kSelf = Scheduler::self().current();
    if (kSelf == nullptr || !kSelf->space.holds() || !kSelf->vmas.holds()) {
        return false;
    }
    if (!mm::demand::FillPage(*kSelf->space, *kSelf->vmas, address, error_code)) {
        if ((error_code & 0x4ULL) != 0) {
            Scheduler::self().exit_current(cinux::syscall::kSigSegv);
        }
        return false;
    }
    return true;
}

}  // namespace

void InstallFaultResolver() {
    arch::isr::InstallPageFaultResolver(&resolve_current_fault);
}

}  // namespace cinux::proc
