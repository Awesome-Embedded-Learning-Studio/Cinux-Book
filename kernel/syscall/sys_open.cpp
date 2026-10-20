#include "kernel/syscall/sys_open.hpp"

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleOpen(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView kFlat = kView.value();

    const arch::IrqGuard     kGuard;
    base::Result<fs::FsNode> node = kFlat.ops->lookup(kFlat.self, kFlat.rest);
    if (!node.ok() && node.error() == base::KernelError::kNotFound &&
        (frame.rsi & kOpenCreat) != 0) {
        node = kFlat.ops->make_file(kFlat.self, kFlat.rest);
    }
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }
    node = kFlat.ops->adopt_open(kFlat.self, node.value(), kFlat.rest);
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }

    const proc::Task* const           kSelf = CurrentTask();
    const base::Result<unsigned long> kFd =
        kSelf->files->alloc(kFlat.ops, kFlat.self, node.value());
    if (!kFd.ok()) {
        return -ErrnoFrom(kFd.error());
    }
    const base::Result<fs::OpenFile*> kLive = kSelf->files->get(kFd.value());
    base::safety::Check(kLive.ok(), "the seat allocated a moment ago is gone");
    kLive.value()->status_flags = frame.rsi & kOpenStatusMask;
    if ((frame.rsi & kOpenCloexec) != 0) {
        base::safety::Check(kSelf->files->set_cloexec(kFd.value(), true).ok(),
                            "close-on-exec refused the seat just handed out");
    }
    return static_cast<long long>(kFd.value());
}

}  // namespace cinux::syscall
