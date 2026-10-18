#include "kernel/syscall/sys_pipe.hpp"

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// Seats both ends of one fresh pipe in the caller's table.
long long seat_pipe_pair(unsigned long long user_fds) {
    if (user_fds == 0 || user_fds >= kUserAddressLimit) {
        return -kEfault;
    }
    auto* const kPipe = new fs::Pipe();
    if (kPipe == nullptr) {
        return -kEnomem;
    }
    kPipe->add_writer();
    const fs::FsNode kReadMark{};
    const fs::FsNode kWriteMark{
        .opaque = reinterpret_cast<void*>(1),
        .type   = fs::InodeType::kFile,
        .ino    = 0,
        .size   = 0,
    };
    const proc::Task* const           kSelf = CurrentTask();
    const base::Result<unsigned long> kReadEnd =
        kSelf->files->alloc(fs::PipeOps(), kPipe, kReadMark);
    if (!kReadEnd.ok()) {
        delete kPipe;
        return -kEnomem;
    }
    const base::Result<unsigned long> kWriteEnd =
        kSelf->files->alloc(fs::PipeOps(), kPipe, kWriteMark);
    if (!kWriteEnd.ok()) {
        cinux::base::safety::Check(kSelf->files->free(kReadEnd.value()).ok(),
                                   "pipe read end vanished while seating the write end");
        delete kPipe;
        return -kEnomem;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned int*>(user_fds);
    kOut[0]          = static_cast<unsigned int>(kReadEnd.value());
    kOut[1]          = static_cast<unsigned int>(kWriteEnd.value());
    return 0;
}

}  // namespace

long long HandlePipe(const SyscallFrame& frame) {
    return seat_pipe_pair(frame.rdi);
}

long long HandlePipe2(const SyscallFrame& frame) {
    if (frame.rsi != 0) {
        return -kEinval;
    }
    return seat_pipe_pair(frame.rdi);
}

long long HandleDup(const SyscallFrame& frame) {
    const base::Result<base::Shared<fs::OpenFile>> kHold = CurrentTask()->files->shared(frame.rdi);
    if (!kHold.ok()) {
        return -kEbadf;
    }
    const base::Result<unsigned long> kCopy = CurrentTask()->files->adopt(kHold.value());
    if (!kCopy.ok()) {
        return -kEmfile;
    }
    return static_cast<long long>(kCopy.value());
}

long long HandleDup2(const SyscallFrame& frame) {
    if (frame.rdi == frame.rsi) {
        const base::Result<fs::OpenFile*> kSame = LiveSlot(frame.rdi);
        return kSame.ok() ? static_cast<long long>(frame.rsi) : -kEbadf;
    }
    const base::Result<void> kClosed = CurrentTask()->files->free(frame.rsi);
    if (!kClosed.ok() && kClosed.error() != base::KernelError::kNotFound) {
        return -kEbadf;
    }
    const base::Result<base::Shared<fs::OpenFile>> kHold = CurrentTask()->files->shared(frame.rdi);
    if (!kHold.ok()) {
        return -kEbadf;
    }
    const base::Result<unsigned long> kCopy =
        CurrentTask()->files->adopt_at(kHold.value(), frame.rsi);
    if (!kCopy.ok()) {
        return -kEmfile;
    }
    return static_cast<long long>(frame.rsi);
}

}  // namespace cinux::syscall
