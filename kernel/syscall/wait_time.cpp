#include "kernel/syscall/wait_time.hpp"

#include "cinux/time.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"
namespace cinux::syscall {
long long ReadTimespec(Word address, Timespec& record) {
    Timespec staged{};
    if (!uaccess::CopyFromUser(&staged, address, sizeof(staged))) {
        return -kEfault;
    }
    if (staged.seconds < 0 || staged.nanoseconds < 0 || staged.nanoseconds >= 1'000'000'000LL) {
        return -kEinval;
    }
    record = staged;
    return 0;
}

long long ReadWaitDeadline(Word address, unsigned long long& deadline) {
    deadline = ~0ULL;
    if (address == 0) {
        return 0;
    }
    Timespec asked{};
    if (const auto kError = ReadTimespec(address, asked); kError != 0) {
        return kError;
    }
    deadline = base::time::DeadlineAfter(
                   base::time::Nanoseconds{MonotonicNanoseconds()},
                   base::time::Seconds{static_cast<unsigned long long>(asked.seconds)},
                   base::time::Nanoseconds{static_cast<unsigned long long>(asked.nanoseconds)})
                   .value;
    return 0;
}

unsigned long long MillisecondDeadline(int milliseconds) {
    if (milliseconds < 0) {
        return ~0ULL;
    }
    return base::time::DeadlineAfter(
               base::time::Nanoseconds{MonotonicNanoseconds()}, base::time::Seconds{0},
               base::time::ToNanoseconds(
                   base::time::Milliseconds{static_cast<unsigned long long>(milliseconds)}))
        .value;
}

}
