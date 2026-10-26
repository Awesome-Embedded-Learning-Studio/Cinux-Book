#include "kernel/syscall/sys_wait.hpp"

#include "cinux/byte_order.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
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

long long HandleWaitid(const SyscallFrame& frame) {
    constexpr unsigned long long kNoHang = 1;
    constexpr unsigned long long kExited = 4;
    if ((frame.r10 & kExited) == 0 || (frame.r10 & ~(kNoHang | kExited)) != 0) {
        return -kEinval;
    }
    long long wanted = -1;
    if (frame.rdi == 1) {
        if (frame.rsi == 0 || frame.rsi > 0x7FFF'FFFF) {
            return -kEinval;
        }
        wanted = static_cast<long long>(frame.rsi);
    } else if (frame.rdi == 2) {
        if (frame.rsi > 0x7FFF'FFFF) {
            return -kEinval;
        }
        wanted = -static_cast<long long>(frame.rsi);
    } else if (frame.rdi != 0) {
        return -kEinval;
    }
    if (frame.rdx != 0 && !UserRangeWritable(frame.rdx, 128)) {
        return -kEfault;
    }
    proc::ChildReceipt receipt{};
    if (!proc::Scheduler::self().wait_child(wanted, (frame.r10 & kNoHang) != 0, receipt)) {
        return -kEchild;
    }
    unsigned char info[128]{};
    if (receipt.pid != 0) {
        const auto kSignal = static_cast<unsigned int>(receipt.code) & 0x7FU;
        base::WriteWord32(info, 0, kSigChld);
        base::WriteWord32(info, 8, kSignal == 0 ? 1 : 2);
        base::WriteWord32(info, 16, static_cast<unsigned int>(receipt.pid));
        base::WriteWord32(info, 24,
                          kSignal == 0 ? static_cast<unsigned int>(receipt.code) >> 8U : kSignal);
    }
    if (frame.rdx != 0 && !uaccess::CopyToUser(frame.rdx, info, sizeof(info))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
