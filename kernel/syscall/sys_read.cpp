#include "kernel/syscall/sys_read.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

/// The read staging buffer: one backend read fills it, the accessor
/// carries it across the user line, and no backend ever sees a
/// ring-3 address.
constexpr unsigned long long kReadStageBytes = 1024;

}  // namespace

long long HandleRead(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    const base::Result<base::Shared<fs::OpenFile>> kSlot = CurrentTask()->files->shared(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const   kFile = kSlot.value().get();
    unsigned char stage[kReadStageBytes];
    const auto    kWant =
        static_cast<unsigned long>(frame.rdx < kReadStageBytes ? frame.rdx : kReadStageBytes);
    for (;;) {
        const base::Result<unsigned long> kGot =
            kFile->ops->read(kFile->backend, kFile->node, kFile->offset, stage, kWant);
        if (kGot.ok()) {
            if (!uaccess::CopyToUser(frame.rsi, stage, kGot.value())) {
                return -kEfault;
            }
            kFile->offset += kGot.value();
            return static_cast<long long>(kGot.value());
        }
        if (kGot.error() != base::KernelError::kWouldBlock ||
            (kFile->status_flags & kOpenNonblock) != 0) {
            return -ErrnoFrom(kGot.error());
        }
        const proc::Task* const kSelf = CurrentTask();
        if (proc::HasPendingSignal(*kSelf)) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
