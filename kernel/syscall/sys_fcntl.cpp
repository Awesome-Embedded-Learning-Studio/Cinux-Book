#include "kernel/syscall/sys_fcntl.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long long kCmdDupFd        = 0;
constexpr unsigned long long kCmdGetFd        = 1;
constexpr unsigned long long kCmdSetFd        = 2;
constexpr unsigned long long kCmdGetFl        = 3;
constexpr unsigned long long kCmdSetFl        = 4;
constexpr unsigned long long kCmdDupFdCloexec = 1030;

/// The one descriptor flag F_GETFD answers and F_SETFD writes: close
/// this number when the next exec commits.
constexpr unsigned long long kFdCloexec = 1;

/// The status half F_SETFL may re-arm: everything but the access mode,
/// which names the open's read/write shape and never moves.
constexpr unsigned long long kSetFlMask = kOpenStatusMask & ~kOpenAccMode;

}  // namespace

namespace {

/// The GETFL/SETFL half: reads or re-arms the status face of the open
/// behind the descriptor; SETFL keeps only the re-armable half.
long long fcntl_status(const SyscallFrame& frame, const proc::Task* self,
                       unsigned long long descriptor) {
    if (frame.rsi == kCmdGetFl) {
        const base::Result<base::Shared<fs::OpenFile>> kLive = self->files->get(descriptor);
        if (!kLive.ok()) {
            return -kEbadf;
        }
        return static_cast<long long>(kLive.value()->status_flags);
    }
    if (frame.rsi == kCmdSetFl) {
        const base::Result<base::Shared<fs::OpenFile>> kLive = self->files->get(descriptor);
        if (!kLive.ok()) {
            return -kEbadf;
        }
        kLive.value()->status_flags =
            (kLive.value()->status_flags & kOpenAccMode) | (frame.rdx & kSetFlMask);
        return 0;
    }
    return -kEinval;
}

/// The GETFD/SETFD half: the per-number seat bit, armed or cleared
/// through the table itself.
long long fcntl_seat(const SyscallFrame& frame, const proc::Task* self,
                     unsigned long long descriptor) {
    if (!self->files->get(descriptor).ok()) {
        return -kEbadf;
    }
    if (frame.rsi == kCmdGetFd) {
        return self->files->cloexec(descriptor) ? static_cast<long long>(kFdCloexec) : 0;
    }
    if (frame.rsi == kCmdSetFd) {
        if (!self->files->set_cloexec(descriptor, (frame.rdx & kFdCloexec) != 0).ok()) {
            return -kEbadf;
        }
        return 0;
    }
    return -kEinval;
}

/// The dup half: copy one descriptor's open to the lowest free seat at
/// or above the caller's floor. F_DUPFD_CLOEXEC arms the new seat's
/// close-on-exec bit on the way out.
long long fcntl_dup(const SyscallFrame& frame, const proc::Task* self,
                    unsigned long long descriptor) {
    if (frame.rdx >= FileDescriptorLimit()) {
        return -kEinval;
    }
    const base::Result<base::Shared<fs::OpenFile>> kHeld = self->files->shared(descriptor);
    if (!kHeld.ok()) {
        return -kEbadf;
    }
    const base::Result<unsigned long> kBorn = self->files->adopt_from(
        kHeld.value(), frame.rdx, frame.rsi == kCmdDupFdCloexec, FileDescriptorLimit());
    if (!kBorn.ok()) {
        return -kEmfile;
    }
    return static_cast<long long>(kBorn.value());
}

}  // namespace

long long HandleFcntl(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    const unsigned long long kDescriptor = frame.rdi;
    const proc::Task* const  kSelf       = CurrentTask();
    if (frame.rsi == kCmdGetFl || frame.rsi == kCmdSetFl) {
        return fcntl_status(frame, kSelf, kDescriptor);
    }
    if (frame.rsi == kCmdGetFd || frame.rsi == kCmdSetFd) {
        return fcntl_seat(frame, kSelf, kDescriptor);
    }
    if (frame.rsi == kCmdDupFd || frame.rsi == kCmdDupFdCloexec) {
        return fcntl_dup(frame, kSelf, kDescriptor);
    }
    return -kEinval;
}

}  // namespace cinux::syscall
