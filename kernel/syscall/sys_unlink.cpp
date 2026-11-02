#include "kernel/syscall/sys_unlink.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleRmdir(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPath(frame.rdi, storage, sizeof(storage)); kBad < 0) {
        return kBad;
    }
    const auto kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const auto& view  = kView.value();
    const auto  kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return -ErrnoFrom(kNode.error());
    }
    if (kNode.value().type != fs::InodeType::kDirectory) {
        return -kEnotdir;
    }
    const auto kRemoved = view.ops->unlink(view.self, view.rest);
    return kRemoved.ok() ? 0 : -ErrnoFrom(kRemoved.error());
}

long long HandleUnlink(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const base::Result<void> kGone =
        kView.value().ops->unlink(kView.value().self, kView.value().rest);
    return kGone.ok() ? 0 : -ErrnoFrom(kGone.error());
}

}  // namespace cinux::syscall
