#include "kernel/syscall/sys_fork.hpp"

#include "kernel/proc/fork.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleFork(const SyscallFrame& frame) {
    proc::Task* const kChild = proc::BuildForkChild(reinterpret_cast<unsigned long long>(&frame));
    if (kChild == nullptr) {
        return -kEnomem;
    }
    proc::Scheduler::self().seat(*kChild);
    return static_cast<long long>(kChild->pid);
}

}  // namespace cinux::syscall
