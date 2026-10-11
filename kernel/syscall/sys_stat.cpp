#include "kernel/syscall/sys_stat.hpp"

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleStat(unsigned long long path, unsigned long long out,
                     [[maybe_unused]] unsigned long long third) {
    if (out >= kUserAddressLimit) {
        return -kEfault;
    }
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(path, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
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
    fs::FsStat               probed{};
    const base::Result<void> kProbed = kFlat.ops->stat(kFlat.self, kNode.value(), probed);
    if (!kProbed.ok()) {
        return -ErrnoFrom(kProbed.error());
    }
    auto* const kSink = reinterpret_cast<SyscallStat*>(out);  // NOLINT(performance-no-int-to-ptr)
    kSink->ino        = probed.ino;
    kSink->mode       = probed.mode;
    kSink->links      = probed.links;
    kSink->size       = probed.size;
    kSink->mtime      = probed.mtime;
    return 0;
}

}  // namespace cinux::syscall
