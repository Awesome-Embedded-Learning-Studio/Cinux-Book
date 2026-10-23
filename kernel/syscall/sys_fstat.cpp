#include "kernel/syscall/sys_fstat.hpp"

#include <cstdint>

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long HandleFstat(const SyscallFrame& frame) {
    const arch::IrqGuard              kGuard;
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    if (kSlot.value()->ops->stat == nullptr) {
        // A backend with no stat face has nothing honest to say; the
        // caller learns the table has no such shape rather than a
        // made-up record.
        return -kEnosys;
    }
    fs::FsStat               probed{};
    const base::Result<void> kProbed =
        kSlot.value()->ops->stat(kSlot.value()->backend, kSlot.value()->node, probed);
    if (!kProbed.ok()) {
        return -ErrnoFrom(kProbed.error());
    }
    SyscallStat sink{};
    sink.ino   = probed.ino;
    sink.mode  = probed.mode;
    sink.links = probed.links;
    sink.size  = static_cast<std::int64_t>(probed.size);
    sink.rdev  = probed.rdev;
    sink.mtime = probed.mtime;
    if (!uaccess::CopyToUser(frame.rsi, &sink, sizeof(sink))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
