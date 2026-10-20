#include "kernel/syscall/sys_write.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long WriteRun(unsigned long long descriptor, unsigned long long source,
                   unsigned long long count) {
    if (descriptor >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (source >= kUserAddressLimit) {
        return -kEfault;
    }
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(descriptor);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const       kFile = kSlot.value();
    auto const* const kSource =
        reinterpret_cast<const void*>(source);  // NOLINT(performance-no-int-to-ptr)
    const base::Result<unsigned long> kPut =
        kFile->ops->write(kFile->backend, kFile->node, kFile->offset, kSource, count);
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
