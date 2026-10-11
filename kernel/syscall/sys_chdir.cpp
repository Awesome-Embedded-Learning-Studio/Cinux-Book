#include "kernel/syscall/sys_chdir.hpp"

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleChdir(unsigned long long path, [[maybe_unused]] unsigned long long second,
                      [[maybe_unused]] unsigned long long third) {
    char            storage[fs::kFsPathMax] = {};
    const long long kLength                 = FoldUserPath(path, storage, fs::kFsPathMax);
    if (kLength < 0) {
        return kLength;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView kFlat = kView.value();

    const arch::IrqGuard           kGuard;
    const base::Result<fs::FsNode> kNode = kFlat.ops->lookup(kFlat.self, kFlat.rest);
    if (!kNode.ok()) {
        return -ErrnoFrom(kNode.error());
    }
    if (kNode.value().type != fs::InodeType::kDirectory) {
        return -kEnotdir;
    }
    base::CopyBytes(CurrentTask()->cwd, storage, static_cast<unsigned long>(kLength) + 1UL);
    return 0;
}

}  // namespace cinux::syscall
