#include "kernel/syscall/sys_write.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

/// The write staging buffer: the accessor carries the caller's bytes
/// across the user line first, then one backend write consumes them,
/// so no backend ever sees a ring-3 address.
constexpr unsigned long long kWriteStageBytes = 1024;

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
    auto* const   kFile = kSlot.value().get();
    unsigned char stage[kWriteStageBytes];
    const auto    kChunk =
        static_cast<unsigned long>(count < kWriteStageBytes ? count : kWriteStageBytes);
    if (!uaccess::CopyFromUser(stage, source, kChunk)) {
        return -kEfault;
    }
    const base::Result<unsigned long> kPut =
        kFile->ops->write(kFile->backend, kFile->node, kFile->offset, stage, kChunk);
    if (!kPut.ok()) {
        return -ErrnoFrom(kPut.error());
    }
    kFile->offset += kPut.value();
    return static_cast<long long>(kPut.value());
}

long long HandleWrite(const SyscallFrame& frame) {
    return WriteRun(frame.rdi, frame.rsi, frame.rdx);
}

}  // namespace cinux::syscall
