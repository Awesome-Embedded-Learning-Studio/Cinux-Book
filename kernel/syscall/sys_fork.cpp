#include "kernel/syscall/sys_fork.hpp"

#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/proc/fork.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleFork(Word /*first*/, Word /*second*/, Word /*third*/) {
    const auto&       bank   = arch::per_cpu::PerCpu::self();
    proc::Task* const kChild = proc::BuildForkChild(bank.syscall_frame);
    if (kChild == nullptr) {
        return -kEnomem;
    }
    proc::Scheduler::self().seat(*kChild);
    return static_cast<long long>(kChild->pid);
}

}  // namespace cinux::syscall
