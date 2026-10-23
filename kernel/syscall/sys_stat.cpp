#include "kernel/syscall/sys_stat.hpp"

#include <cstdint>

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long HandleStat(const SyscallFrame& frame) {
    if (!UserRangeWritable(frame.rsi, sizeof(SyscallStat))) {
        return -kEfault;
    }
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
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
