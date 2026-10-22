#include "kernel/syscall/sys_read.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

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
    auto* const kFile = kSlot.value().get();
    auto* const kSink = reinterpret_cast<void*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
    for (;;) {
        const base::Result<unsigned long> kGot =
            kFile->ops->read(kFile->backend, kFile->node, kFile->offset, kSink, frame.rdx);
        if (kGot.ok()) {
            kFile->offset += kGot.value();
            return static_cast<long long>(kGot.value());
        }
        if (kGot.error() != base::KernelError::kWouldBlock ||
            (kFile->status_flags & kOpenNonblock) != 0) {
            return -ErrnoFrom(kGot.error());
        }
        const proc::Task* const kSelf = CurrentTask();
        if (kSelf->signals.pending.has(~kSelf->signals.blocked)) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace cinux::syscall
