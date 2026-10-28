#include "kernel/syscall/sys_rename.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
namespace cinux::syscall {

namespace {
/// Cross-device link: the two names live on different filesystems.
constexpr int kExdev = 18;
}
long long HandleRename(const SyscallFrame& frame) {
    char source[fs::kFsPathMax]{};
    char target[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPath(frame.rdi, source, sizeof(source)); kBad < 0) {
        return kBad;
    }
    if (const auto kBad = FoldUserPath(frame.rsi, target, sizeof(target)); kBad < 0) {
        return kBad;
    }
    const auto kFrom = fs::FileWorld::self().resolve(source);
    const auto kTo   = fs::FileWorld::self().resolve(target);
    if (!kFrom.ok()) {
        return -ErrnoFrom(kFrom.error());
    }
    if (!kTo.ok()) {
        return -ErrnoFrom(kTo.error());
    }
    if (kFrom.value().self != kTo.value().self || kFrom.value().ops != kTo.value().ops) {
        return -kExdev;
    }
    if (kFrom.value().ops->rename == nullptr) {
        return -kEopnotsupp;
    }
    const auto kMoved =
        kFrom.value().ops->rename(kFrom.value().self, kFrom.value().rest, kTo.value().rest);
    return kMoved.ok() ? 0 : -ErrnoFrom(kMoved.error());
}
long long HandleChmod(const SyscallFrame& frame) {
    char path[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPath(frame.rdi, path, sizeof(path)); kBad < 0) {
        return kBad;
    }
    const auto kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const auto& view = kView.value();
    if (view.ops->chmod == nullptr) {
        return -kEopnotsupp;
    }
    const auto kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return -ErrnoFrom(kNode.error());
    }
    const auto kChanged =
        view.ops->chmod(view.self, kNode.value(), static_cast<unsigned int>(frame.rsi));
    return kChanged.ok() ? 0 : -ErrnoFrom(kChanged.error());
}
}
