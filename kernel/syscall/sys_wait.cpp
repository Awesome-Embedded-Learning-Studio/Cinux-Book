#include "kernel/syscall/sys_wait.hpp"

#include "cinux/ptr.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleWait4(Word pid_word, Word status_word, Word third) {
    static_cast<void>(third);
    if (status_word >= kUserAddressLimit) {
        return -kEfault;
    }
    const auto kWanted = static_cast<long long>(pid_word);
    if (kWanted != -1 && kWanted <= 0) {
        return -kEinval;
    }
    proc::ChildReceipt receipt{};
    if (!proc::Scheduler::self().wait_child(kWanted, receipt)) {
        return -kEchild;
    }
    if (status_word != 0) {
        *cinux::base::PtrAt<int>(status_word) = static_cast<int>(receipt.code);
    }
    return static_cast<long long>(receipt.pid);
}

}  // namespace cinux::syscall
