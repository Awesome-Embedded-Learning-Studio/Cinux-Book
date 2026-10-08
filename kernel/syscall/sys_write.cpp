#include "kernel/syscall/sys_write.hpp"

#include "cinux/result.hpp"
#include "kernel/console/console.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

long long console_from(unsigned long long buffer, unsigned long long count) {
    auto const* text = reinterpret_cast<const char*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    for (unsigned long long i = 0; i < count; ++i) {
        console::PutChar(text[i]);
    }
    return static_cast<long long>(count);
}

}  // namespace

long long HandleWrite(unsigned long long descriptor, unsigned long long buffer,
                      unsigned long long count) {
    if (descriptor >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (buffer >= kUserAddressLimit) {
        return -kEfault;
    }
    if (descriptor == 1 || descriptor == 2) {
        return console_from(buffer, count);
    }
    const base::Result<fs::FileSlot*> kSlot = LiveSlot(descriptor);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const       kFile = kSlot.value();
    auto const* const kSource =
        reinterpret_cast<const void*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    const base::Result<unsigned long> kPut =
        kFile->ops->write(kFile->backend, kFile->node, kFile->offset, kSource, count);
    if (!kPut.ok()) {
        return -ErrnoFrom(kPut.error());
    }
    kFile->offset += kPut.value();
    return static_cast<long long>(kPut.value());
}

}  // namespace cinux::syscall
