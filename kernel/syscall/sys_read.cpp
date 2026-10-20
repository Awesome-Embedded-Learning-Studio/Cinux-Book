#include "kernel/syscall/sys_read.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
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
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const kFile = kSlot.value();
    auto* const kSink = reinterpret_cast<void*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
    const base::Result<unsigned long> kGot =
        kFile->ops->read(kFile->backend, kFile->node, kFile->offset, kSink, frame.rdx);
    if (!kGot.ok()) {
        return -ErrnoFrom(kGot.error());
    }
    kFile->offset += kGot.value();
    return static_cast<long long>(kGot.value());
}

}  // namespace cinux::syscall
