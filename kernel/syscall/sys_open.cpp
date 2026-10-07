#include "kernel/syscall/sys_open.hpp"

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleOpen(unsigned long long path, unsigned long long flags,
                     [[maybe_unused]] unsigned long long third) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = CopyUserPath(path, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView kFlat = kView.value();

    const arch::IrqGuard     kGuard;
    base::Result<fs::FsNode> node = kFlat.ops->lookup(kFlat.self, kFlat.rest);
    if (!node.ok() && node.error() == base::KernelError::kNotFound && (flags & kOpenCreat) != 0) {
        node = kFlat.ops->make_file(kFlat.self, kFlat.rest);
    }
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }

    const base::Result<unsigned long> kFd =
        CurrentTask()->files.alloc(kFlat.ops, kFlat.self, node.value());
    if (!kFd.ok()) {
        return -ErrnoFrom(kFd.error());
    }
    return static_cast<long long>(kFd.value());
}

}  // namespace cinux::syscall
