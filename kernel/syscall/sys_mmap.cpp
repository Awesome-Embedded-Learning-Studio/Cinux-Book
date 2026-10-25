#include "kernel/syscall/sys_mmap.hpp"

#include <utility>

#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
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
    const unsigned long long kAddress = frame.rdi;
    const unsigned long long kLength  = frame.rsi;
    const unsigned long long kProt    = frame.rdx;
    const unsigned long long kFlags   = frame.r10;
    const unsigned long long kFd      = frame.r8;
    const unsigned long long kOffset  = frame.r9;
    const bool               kFixed   = (kFlags & kMapFixed) != 0;
    if (kLength == 0 || kLength > kUserAddressLimit - cinux::arch::page::kSize ||
        (kFixed && kAddress % cinux::arch::page::kSize != 0) ||
        kOffset % cinux::arch::page::kSize != 0 ||
        (kFlags & (kMapPrivate | kMapShared)) != kMapPrivate ||
        (kProt & ~(kProtRead | kProtWrite | kProtExec)) != 0) {
        return -kEinval;
    }
    const unsigned long kBytes = cinux::base::math::AlignUp(kLength, cinux::arch::page::kSize);
    unsigned long       start  = 0;
    base::Shared<fs::OpenFile> backing;
    mm::VmaRegion              region{};
    region.readable   = (kProt & kProtRead) != 0;
    region.writable   = (kProt & kProtWrite) != 0;
    region.executable = (kProt & kProtExec) != 0;
    if ((kFlags & kMapAnonymous) == 0) {
        const base::Result<base::Shared<fs::OpenFile>> kSlot = LiveSlot(kFd);
        if (!kSlot.ok()) {
            return -ErrnoFrom(kSlot.error());
        }
        backing            = kSlot.value();
        region.kind        = mm::VmaKind::kFile;
        region.backing     = kSlot.value()->node;
        region.file_ops    = kSlot.value()->ops;
        region.file_self   = kSlot.value()->backend;
        region.open_file   = std::move(backing);
        region.file_offset = kOffset;
    } else {
        region.kind = mm::VmaKind::kAnonymous;
    }
    if (kFixed) {
        if (kAddress < mm::kUserNullTop || kAddress >= kUserAddressLimit ||
            kBytes > kUserAddressLimit - kAddress) {
            return -kEinval;
        }
        start                 = static_cast<unsigned long>(kAddress);
        const auto kReclaimed = ReclaimUserSpan(*kSelf, start, kBytes);
        if (!kReclaimed.ok()) {
            return -ErrnoFrom(kReclaimed.error());
        }
    } else {
        const base::Result<unsigned long> kSpot =
            kSelf->vmas->find_free_spot(kSelf->vmas->mmap_ceiling(), kBytes);
        if (!kSpot.ok()) {
            return -kEnomem;
        }
        start = kSpot.value();
    }
    region.start = start;
    region.end   = start + kBytes;
    if (!kSelf->vmas->register_range(region).ok()) {
        return -kEnomem;
    }
    return static_cast<long long>(start);
}

}  // namespace cinux::syscall
