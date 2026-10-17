#include "kernel/syscall/sys_read.hpp"

#include "cinux/result.hpp"
#include "kernel/driver/keyboard/keyboard.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

long long keyboard_into(unsigned long long buffer, unsigned long long count) {
    auto* out = reinterpret_cast<char*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    for (unsigned long long got = 0; got < count; ++got) {
        char glyph = driver::Keyboard::self().take(true);
        if (glyph == '\r') {
            glyph = '\n';
        }
        out[got] = glyph;
        if (glyph == '\n') {
            return static_cast<long long>(got + 1);
        }
    }
    return static_cast<long long>(count);
}

}  // namespace

long long HandleRead(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    if (frame.rdi == 0) {
        return keyboard_into(frame.rsi, frame.rdx);
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
