#include "kernel/syscall/sys_lseek.hpp"

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// The whence codes the ABI defines: one anchors at the cursor, two past
/// the final byte, and zero — the default anchor — at the first byte; a
/// fourth or higher value is not a position question and the call
/// refuses it.
constexpr unsigned long long kSeekCur = 1;
constexpr unsigned long long kSeekEnd = 2;

}  // namespace

long long HandleLseek(const SyscallFrame& frame) {
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    if (frame.rdx > kSeekEnd) {
        return -kEinval;
    }
    const base::Result<base::Shared<fs::OpenFile>> kSlot = CurrentTask()->files->shared(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const kFile  = kSlot.value().get();
    long long   anchor = 0;
    if (frame.rdx == kSeekCur) {
        anchor = static_cast<long long>(kFile->offset);
    } else if (frame.rdx == kSeekEnd) {
        if (kFile->ops->stat == nullptr) {
            return -kEspipe;
        }
        fs::FsStat probed{};
        if (!kFile->ops->stat(kFile->backend, kFile->node, probed).ok()) {
            return -kEio;
        }
        anchor = static_cast<long long>(probed.size);
    }
    const long long kTarget = anchor + static_cast<long long>(frame.rsi);
    if (kTarget < 0) {
        return -kEinval;
    }
    kFile->offset = static_cast<unsigned long>(kTarget);
    return kTarget;
}

}  // namespace cinux::syscall
