#include "kernel/syscall/sys_fsync.hpp"

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleFsync(const SyscallFrame& frame) {
    const auto kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    fs::FsStat        probed{};
    const auto* const kFile = kSlot.value().get();
    const auto        kStat = kFile->ops->stat(kFile->backend, kFile->node, probed);
    if (!kStat.ok()) {
        return -ErrnoFrom(kStat.error());
    }
    const auto kType = base::bit::BitMask{probed.mode}.extract({.low = 12, .width = 4});
    if (kType != 8 && kType != 4) {
        return -kEinval;
    }
    return fs::FileWorld::self().sync() ? 0 : -kEio;
}

}
