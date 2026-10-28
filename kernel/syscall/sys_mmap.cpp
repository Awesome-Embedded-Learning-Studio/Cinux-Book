#include "kernel/syscall/sys_mmap.hpp"

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

namespace {

/// One backed span's first byte and length, as mmap received them.
struct FileSpan {
    unsigned long long offset;
    unsigned long long length;
};

/// @brief Whether a file's frames answer for both edges of one span.
[[nodiscard]] bool frames_reach(const fs::OpenFile& file, FileSpan span) {
    return file.ops->frame_at != nullptr &&
           file.ops->frame_at(file.backend, file.node, span.offset).ok() &&
           file.ops->frame_at(file.backend, file.node, span.offset + span.length - 1U).ok();
}

/// @brief Shapes one file-backed promise: shared wants a device's frames,
///        private rides the file's bytes.
[[nodiscard]] long long dress_file_region(unsigned long long descriptor, FileSpan span,
                                          bool shared_wanted, mm::VmaRegion& region) {
    const base::Result<base::Shared<fs::OpenFile>> kSlot = LiveSlot(descriptor);
    if (!kSlot.ok()) {
        return -ErrnoFrom(kSlot.error());
    }
    if (shared_wanted && !frames_reach(*kSlot.value(), span)) {
        return -kEinval;
    }
    region.kind        = shared_wanted ? mm::VmaKind::kDevice : mm::VmaKind::kFile;
    region.backing     = kSlot.value()->node;
    region.file_ops    = kSlot.value()->ops;
    region.file_self   = kSlot.value()->backend;
    region.open_file   = kSlot.value();
    region.file_offset = span.offset;
    return 0;
}

}  // namespace

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
    const bool               kShared  = (kFlags & (kMapPrivate | kMapShared)) == kMapShared;
    if (kLength == 0 || kLength > kUserAddressLimit - cinux::arch::page::kSize ||
        (kFixed && kAddress % cinux::arch::page::kSize != 0) ||
        kOffset % cinux::arch::page::kSize != 0 || ((kFlags & (kMapPrivate | kMapShared)) == 0) ||
        (kProt & ~(kProtRead | kProtWrite | kProtExec)) != 0) {
        return -kEinval;
    }
    const unsigned long kBytes = cinux::base::math::AlignUp(kLength, cinux::arch::page::kSize);
    unsigned long       start  = 0;
    mm::VmaRegion       region{};
    region.readable   = (kProt & kProtRead) != 0;
    region.writable   = (kProt & kProtWrite) != 0;
    region.executable = (kProt & kProtExec) != 0;
    if ((kFlags & kMapAnonymous) == 0) {
        const long long kDressed =
            dress_file_region(kFd, FileSpan{.offset = kOffset, .length = kLength}, kShared, region);
        if (kDressed != 0) {
            return kDressed;
        }
    } else {
        if (kShared) {
            return -kEinval;
        }
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
