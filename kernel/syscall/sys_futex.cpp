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
    const Word kUaddr = frame.rdi;
    const Word kOp    = frame.rsi;
    const Word kVal   = frame.rdx;
    if ((kUaddr & 3ULL) != 0ULL) {
        return -kEinval;
    }
    if (kOp == kFutexWait) {
        if (!UserRangeReadable(kUaddr, sizeof(unsigned int))) {
            return -kEfault;
        }
        if (cinux::proc::futex::WaitOnUserWord(kUaddr, static_cast<unsigned int>(kVal)) ==
            base::KernelError::kWouldBlock) {
            return -kEagain;
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
