#include "kernel/syscall/sys_readlink.hpp"

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

long long readlink_path(const char* path, Word destination, Word capacity) {
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView& flat = kView.value();

    const arch::IrqGuard           kGuard;
    const base::Result<fs::FsNode> kNode = flat.ops->lookup(flat.self, flat.rest);
    if (!kNode.ok()) {
        return -ErrnoFrom(kNode.error());
    }
    if (kNode.value().ino != fs::kProcExeIno || kNode.value().type != fs::InodeType::kFile) {
        return -kEinval;
    }
    const proc::Task* const kSelf  = CurrentTask();
    unsigned long long      length = 0;
    while (length + 1 < sizeof(kSelf->exe) && kSelf->exe[length] != '\0') {
        ++length;
    }
    const unsigned long long kGive = length < capacity ? length : capacity;
    if (!uaccess::CopyToUser(destination, kSelf->exe, kGive)) {
        return -kEfault;
    }
    return static_cast<long long>(kGive);
}
}  // namespace

long long HandleReadlinkat(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPathAt(frame.rdi, frame.rsi, storage); kBad < 0) {
        return kBad;
    }
    return readlink_path(storage, frame.rdx, frame.r10);
}

long long HandleReadlink(const SyscallFrame& frame) {
    if (frame.rsi == 0 || frame.rdx == 0) {
        return -kEfault;
    }
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    return readlink_path(storage, frame.rsi, frame.rdx);
}


}  // namespace cinux::syscall
