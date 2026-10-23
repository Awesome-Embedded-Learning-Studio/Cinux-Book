#include "kernel/syscall/sys_wait.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long HandleWait4(const SyscallFrame& frame) {
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    const auto                   kWanted  = static_cast<long long>(frame.rdi);
    constexpr unsigned long long kWNoHang = 1;
    const bool                   kNoHang  = (frame.rdx & kWNoHang) != 0;
    proc::ChildReceipt           receipt{};
    if (!proc::Scheduler::self().wait_child(kWanted, kNoHang, receipt)) {
        return -kEchild;
    }
    if (receipt.pid == 0) {
        return 0;
    }
    if (frame.rsi != 0) {
        const int kStatus = static_cast<int>(receipt.code);
        if (!uaccess::CopyToUser(frame.rsi, &kStatus, sizeof(kStatus))) {
            return -kEfault;
        }
    }
    return static_cast<long long>(receipt.pid);
}

}  // namespace cinux::syscall
