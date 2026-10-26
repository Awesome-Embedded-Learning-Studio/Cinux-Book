#include "kernel/syscall/sys_tgkill.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {
namespace {

long long signal_thread(unsigned long long tid, unsigned long long tgid, int number) {
    if (static_cast<long long>(tid) <= 0 || number < 0 || number > static_cast<int>(kSignalCount)) {
        return -kEinval;
    }
    long long result = -kEsrch;
    proc::Scheduler::self().with_task(tid, [&](proc::Task& target) {
        if (tgid == 0 || target.tgid == tgid) {
            result = number == 0 || proc::SendSignal(target, number) ? 0 : -kEinval;
        }
    });
    return result;
}

}

long long HandleTgkill(const SyscallFrame& frame) {
    if (static_cast<long long>(frame.rdi) <= 0) {
        return -kEinval;
    }
    return signal_thread(frame.rsi, frame.rdi, static_cast<int>(frame.rdx));
}

long long HandleTkill(const SyscallFrame& frame) {
    return signal_thread(frame.rdi, 0, static_cast<int>(frame.rsi));
}

}
