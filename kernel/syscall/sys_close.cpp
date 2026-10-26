#include "kernel/syscall/sys_close.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleClose(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    const base::Result<void> kFreed = CurrentTask()->files->free(frame.rdi);
    return kFreed.ok() ? 0 : -kEbadf;
}

long long HandleCloseRange(const SyscallFrame& frame) {
    constexpr unsigned int kUnshare = 2;
    constexpr unsigned int kCloexec = 4;
    const auto             kFirst   = static_cast<unsigned int>(frame.rdi);
    const auto             kLast    = static_cast<unsigned int>(frame.rsi);
    const auto             kFlags   = static_cast<unsigned int>(frame.rdx);
    if (kFirst > kLast || (kFlags & ~(kUnshare | kCloexec)) != 0) {
        return -kEinval;
    }
    auto* const kSelf = CurrentTask();
    if ((kFlags & kUnshare) != 0) {
        const auto kCopy = kSelf->files->clone();
        if (!kCopy.holds()) {
            return -kEnomem;
        }
        kSelf->files = kCopy;
    }
    const auto kCapacity = kSelf->files->capacity();
    for (unsigned long descriptor = kFirst; descriptor <= kLast && descriptor < kCapacity;
         ++descriptor) {
        if (!kSelf->files->get(descriptor).ok()) {
            continue;
        }
        const auto kChanged = (kFlags & kCloexec) != 0 ? kSelf->files->set_cloexec(descriptor, true)
                                                       : kSelf->files->free(descriptor);
        if (!kChanged.ok()) {
            return -kEbadf;
        }
    }
    return 0;
}

}  // namespace cinux::syscall
