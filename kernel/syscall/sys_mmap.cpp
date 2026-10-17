#include "kernel/syscall/sys_mmap.hpp"

#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleMmap(const SyscallFrame& frame) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || !kSelf->vmas.holds()) {
        return -kEnomem;
    }
    const unsigned long long kLength = frame.rsi;
    const unsigned long long kProt   = frame.rdx;
    const unsigned long long kFlags  = frame.r10;
    const unsigned long long kFd     = frame.r8;
    const unsigned long long kOffset = frame.r9;
    if (kLength == 0 || (kFlags & kMapFixed) != 0) {
        return -kEinval;
    }
    const unsigned long kBytes = cinux::base::math::AlignUp(kLength, cinux::arch::page::kSize);
    const base::Result<unsigned long> kSpot = kSelf->vmas->find_free_spot(mm::kUserMmapTop, kBytes);
    if (!kSpot.ok()) {
        return -kEnomem;
    }
    mm::VmaRegion region{};
    region.start      = kSpot.value();
    region.end        = kSpot.value() + kBytes;
    region.readable   = (kProt & kProtRead) != 0;
    region.writable   = (kProt & kProtWrite) != 0;
    region.executable = (kProt & kProtExec) != 0;
    if ((kFlags & kMapAnonymous) != 0) {
        region.kind = mm::VmaKind::kAnonymous;
    } else {
        const base::Result<fs::OpenFile*> kSlot = LiveSlot(kFd);
        if (!kSlot.ok()) {
            return -ErrnoFrom(kSlot.error());
        }
        if (region.writable) {
            return -kEinval;
        }
        region.kind        = mm::VmaKind::kFile;
        region.backing     = kSlot.value()->node;
        region.file_ops    = kSlot.value()->ops;
        region.file_self   = kSlot.value()->backend;
        region.file_offset = kOffset;
    }
    if (!kSelf->vmas->register_range(region).ok()) {
        return -kEnomem;
    }
    return static_cast<long long>(kSpot.value());
}

}  // namespace cinux::syscall
