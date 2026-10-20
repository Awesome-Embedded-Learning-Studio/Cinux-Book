#include "kernel/syscall/sys_tgkill.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleTgkill(const SyscallFrame& frame) {
    if (frame.rdi == 0 || frame.rdx == 0) {
        return -kEinval;
    }
    proc::Task* const kTarget = proc::Scheduler::self().find_by_pid(frame.rdi);
    if (kTarget == nullptr) {
        return -kEsrch;
    }
    if (!proc::SendSignal(*kTarget, static_cast<int>(frame.rdx))) {
        return -kEinval;
    }
    return 0;
}

}  // namespace cinux::syscall
