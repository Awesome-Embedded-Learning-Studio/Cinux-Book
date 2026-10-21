#include "kernel/syscall/sys_tgkill.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleTgkill(const SyscallFrame& frame) {
    if (frame.rdi == 0 || frame.rdx == 0) {
        return -kEinval;
    }
    long long result = -kEsrch;
    proc::Scheduler::self().with_task(frame.rdi, [&](proc::Task& target) {
        result = proc::SendSignal(target, static_cast<int>(frame.rdx)) ? 0 : -kEinval;
    });
    return result;
}

}  // namespace cinux::syscall
