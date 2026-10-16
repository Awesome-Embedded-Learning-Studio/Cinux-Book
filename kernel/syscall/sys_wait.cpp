#include "kernel/syscall/sys_wait.hpp"

#include "cinux/ptr.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"

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
        *cinux::base::PtrAt<int>(frame.rsi) = static_cast<int>(receipt.code);
    }
    return static_cast<long long>(receipt.pid);
}

}  // namespace cinux::syscall
