#include "kernel/syscall/sys_write.hpp"

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {
constexpr base::bit::BitRange kModeTypeSegment{.low = 12, .width = 4};
constexpr unsigned long       kModeRegular = 8;
bool                          regular_file(unsigned long mode) {
    return base::bit::BitMask{mode}.extract(kModeTypeSegment) == kModeRegular;
}


/// The write staging buffer: the accessor carries the caller's bytes
/// across the user line first, then one backend write consumes them,
/// so no backend ever sees a ring-3 address.
constexpr unsigned long long kWriteStageBytes = 1024;

using WriteFn = decltype(fs::FsOps::write);

/// Offers the staged bytes once, then waits out a full counter or busy
/// peer: only O_NONBLOCK turns a busy write into EAGAIN, and a pending
/// signal breaks the wait.
// NOLINTNEXTLINE(readability-function-size) the waiting loop names every exit
long long put_until_settled(fs::OpenFile& file, WriteFn write, const unsigned char* stage,
                            unsigned long chunk, bool append) {
    for (;;) {
        const auto kPut = append
                              ? file.ops->append(file.backend, file.node, file.offset, stage, chunk)
                              : write(file.backend, file.node, file.offset, stage, chunk);
        if (kPut.ok()) {
            if (!append) {
                file.offset += kPut.value();
            }
            return static_cast<long long>(kPut.value());
        }
        if (kPut.error() != base::KernelError::kWouldBlock ||
            (file.status_flags & kOpenNonblock) != 0) {
            return -ErrnoFrom(kPut.error());
        }
        if (proc::HasPendingSignal(*CurrentTask())) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

}  // namespace

long long WriteRun(unsigned long long descriptor, unsigned long long source,
                   unsigned long long count) {
    if (descriptor >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (source >= kUserAddressLimit) {
        return -kEfault;
    }
    const base::Result<base::Shared<fs::OpenFile>> kSlot = LiveSlot(descriptor);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    // NOLINTNEXTLINE(misc-const-correctness) the settle loop advances this open's cursor
    auto* const   kFile = kSlot.value().get();
    unsigned char stage[kWriteStageBytes];
    const auto    kChunk =
        static_cast<unsigned long>(count < kWriteStageBytes ? count : kWriteStageBytes);
    if (!uaccess::CopyFromUser(stage, source, kChunk)) {
        return -kEfault;
    }
    bool append = false;
    if ((kFile->status_flags & kOpenAppend) != 0 && kFile->ops->stat != nullptr) {
        fs::FsStat probed{};
        const auto kStat = kFile->ops->stat(kFile->backend, kFile->node, probed);
        if (!kStat.ok()) {
            return -ErrnoFrom(kStat.error());
        }
        append = regular_file(probed.mode);
    }
    const bool kTryWrite =
        (kFile->status_flags & kOpenNonblock) != 0 && kFile->ops->try_write != nullptr;
    const auto kWrite = kTryWrite ? kFile->ops->try_write : kFile->ops->write;
    if (append && kFile->ops->append == nullptr) {
        return -kEopnotsupp;
    }
    return put_until_settled(*kFile, kWrite, stage, kChunk, append);
}

long long HandleWrite(const SyscallFrame& frame) {
    return WriteRun(frame.rdi, frame.rsi, frame.rdx);
}

}  // namespace cinux::syscall
