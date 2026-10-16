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

long long HandleWrite(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    if (frame.rdi == 1 || frame.rdi == 2) {
        return console_from(frame.rsi, frame.rdx);
    }
    const base::Result<fs::FileSlot*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const       kFile = kSlot.value();
    auto const* const kSource =
        reinterpret_cast<const void*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
    const base::Result<unsigned long> kPut =
        kFile->ops->write(kFile->backend, kFile->node, kFile->offset, kSource, frame.rdx);
    if (!kPut.ok()) {
        return -ErrnoFrom(kPut.error());
    }
    kFile->offset += kPut.value();
    return static_cast<long long>(kPut.value());
}

}  // namespace cinux::syscall
