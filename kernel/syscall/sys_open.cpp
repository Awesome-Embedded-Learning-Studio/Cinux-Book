#include "kernel/syscall/sys_open.hpp"

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
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
    const fs::FsView& flat = kView.value();

    const arch::IrqGuard     kGuard;
    base::Result<fs::FsNode> node = flat.ops->lookup(flat.self, flat.rest);
    if (!node.ok() && node.error() == base::KernelError::kNotFound &&
        (frame.rsi & kOpenCreat) != 0) {
        node = flat.ops->make_file(flat.self, flat.rest);
    }
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }
    node = flat.ops->adopt_open(flat.self, node.value(), flat.rest);
    if (!node.ok()) {
        return -ErrnoFrom(node.error());
    }

    const proc::Task* const           kSelf = CurrentTask();
    const base::Result<unsigned long> kFd =
        kSelf->files->alloc(flat.ops, flat.self, node.value(),
                            {
                                .status   = frame.rsi & kOpenStatusMask,
                                .cloexec  = (frame.rsi & kOpenCloexec) != 0,
                                .lifetime = flat.lifetime,
                                .limit    = FileDescriptorLimit(),
                            });
    if (!kFd.ok()) {
        return -ErrnoFrom(kFd.error());
    }
    constexpr Word kOpenTrunc = 0x200;
    if ((frame.rsi & kOpenTrunc) != 0 && (frame.rsi & kOpenAccMode) != 0 &&
        node.value().type == fs::InodeType::kFile) {
        const auto kCut = flat.ops->truncate(flat.self, node.value());
        if (!kCut.ok()) {
            cinux::base::safety::Check(kSelf->files->free(kFd.value()).ok(),
                                       "open lost its descriptor");
            return -ErrnoFrom(kCut.error());
        }
    }

    constexpr Word kOpenNoctty = 0x100;
    if (node.value().type == fs::InodeType::kCharDevice && (frame.rsi & kOpenNoctty) == 0 &&
        kSelf->controlling_tty == nullptr && kSelf->sid == kSelf->tgid &&
        flat.ops->ioctl != nullptr) {
        flat.ops->ioctl(flat.self, node.value(), driver::kTtyTiocsctty, 0);
    }
    return static_cast<long long>(kFd.value());
}

}  // namespace cinux::syscall
