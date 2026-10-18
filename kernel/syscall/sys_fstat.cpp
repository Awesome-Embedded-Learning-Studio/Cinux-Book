#include "kernel/syscall/sys_fstat.hpp"

#include <cstdint>

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleFstat(const SyscallFrame& frame) {
    if (!UserRangeWritable(frame.rsi, sizeof(SyscallStat))) {
        return -kEfault;
    }
    if (frame.rdi <= 2) {
        return -kEinval;
    }
    const arch::IrqGuard              kGuard;
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    fs::FsStat               probed{};
    const base::Result<void> kProbed =
        kSlot.value()->ops->stat(kSlot.value()->backend, kSlot.value()->node, probed);
    if (!kProbed.ok()) {
        return -ErrnoFrom(kProbed.error());
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kSink = reinterpret_cast<SyscallStat*>(frame.rsi);
    *kSink            = {};
    kSink->ino        = probed.ino;
    kSink->mode       = probed.mode;
    kSink->links      = probed.links;
    kSink->size       = static_cast<std::int64_t>(probed.size);
    kSink->mtime      = probed.mtime;
    return 0;
}

}  // namespace cinux::syscall
