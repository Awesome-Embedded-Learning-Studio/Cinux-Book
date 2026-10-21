#include "kernel/syscall/sys_kill.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// Whether the kernel serves this signal number at all; the group
/// paths need it up front so an unusable number answers EINVAL
/// instead of being mistaken for nobody home.
bool number_served(int number) {
    return number >= 1 && number <= static_cast<int>(kSignalCount);
}

}  // namespace

long long HandleKill(const SyscallFrame& frame) {
    const int        kNumber   = static_cast<int>(frame.rsi);
    const auto       kNamedPid = static_cast<long long>(frame.rdi);
    proc::Scheduler& scheduler = proc::Scheduler::self();
    if (kNamedPid > 0) {
        long long result = -kEsrch;
        scheduler.with_task(static_cast<unsigned long long>(kNamedPid), [&](proc::Task& target) {
            result = (kNumber == 0 || proc::SendSignal(target, kNumber)) ? 0 : -kEinval;
        });
        return result;
    }
    // Zero names the caller's own group, -1 every other task on the
    // roll (the bring-up init has no parent, so it is not on the roll
    // to begin with), and anything below names the group at -pid.
    if (kNumber != 0 && !number_served(kNumber)) {
        return -kEinval;
    }
    proc::Task* const        kSelf  = CurrentTask();
    const bool               kAlone = kNamedPid == -1;
    const unsigned long long kGroup =
        kNamedPid == 0 ? kSelf->pgid : static_cast<unsigned long long>(-kNamedPid);
    unsigned long long heard = 0;
    scheduler.for_each_task([&](proc::Task& scan) {
        const bool kReach = kAlone ? &scan != kSelf : scan.pgid == kGroup;
        if (kReach && (kNumber == 0 || proc::SendSignal(scan, kNumber))) {
            ++heard;
        }
    });
    if (heard == 0) {
        return -kEsrch;
    }
    return 0;
}

}  // namespace cinux::syscall
