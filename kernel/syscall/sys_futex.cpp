#include "kernel/syscall/sys_futex.hpp"

#include <cinux/result.hpp>

#include "kernel/proc/futex.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// futex op: park while the word holds the expected value.
inline constexpr Word kFutexWait = 0;
/// futex op: wake up to val waiters of the word.
inline constexpr Word kFutexWake = 1;

}  // namespace

long long HandleFutex(const SyscallFrame& frame) {
    const Word     kUaddr   = frame.rdi;
    constexpr Word kPrivate = 128;
    const Word     kOp      = frame.rsi & ~kPrivate;
    const Word     kVal     = frame.rdx;
    if ((kUaddr & 3ULL) != 0ULL) {
        return -kEinval;
    }
    if (kOp == kFutexWait) {
        if (!UserRangeReadable(kUaddr, sizeof(unsigned int))) {
            return -kEfault;
        }
        const auto kWait =
            cinux::proc::futex::WaitOnUserWord(kUaddr, static_cast<unsigned int>(kVal));
        if (kWait != base::KernelError::kOk) {
            return kWait == base::KernelError::kWouldBlock ? -kEagain : -kEfault;
        }
        return 0;
    }
    if (kOp == kFutexWake) {
        if (!UserRangeReadable(kUaddr, sizeof(unsigned int))) {
            return -kEfault;
        }
        return cinux::proc::futex::WakeUserWord(kUaddr, kVal);
    }
    return -kEnosys;
}

}  // namespace cinux::syscall
