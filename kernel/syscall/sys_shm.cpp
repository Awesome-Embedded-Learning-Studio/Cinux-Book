#include "kernel/syscall/sys_shm.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

#include "cinux/addr.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/shared.hpp"
#include "cinux/singleton.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/shared_pages.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {
namespace {

using base::operator""_MiB;
constexpr unsigned long kSegmentLimit    = 16;
constexpr unsigned long kSegmentBytesMax = 1_MiB;
constexpr Word          kIpcCreate       = 01000;
constexpr Word          kIpcExclusive    = 02000;
constexpr Word          kReadOnly        = 010000;
constexpr Word          kRound           = 020000;

struct Segment {
    base::Shared<mm::SharedPages> pages;
    std::uint32_t                 key{};
    std::uint32_t                 uid{};
    std::uint32_t                 gid{};
    std::uint32_t                 mode{};
    std::uint32_t                 creator{};
    unsigned long                 bytes{};
    unsigned long                 identity{};
};

class ShmWorld : public base::Singleton<ShmWorld> {
    friend class base::Singleton<ShmWorld>;

public:
    proc::Mutex   gate;
    Segment*      entries[kSegmentLimit]{};
    unsigned long next_identity = 1;

    [[nodiscard]] Segment* find(unsigned long identity) const {
        for (Segment* segment : entries) {
            if (segment != nullptr && segment->identity == identity) {
                return segment;
            }
        }
        return nullptr;
    }

private:
    ShmWorld() = default;
};

bool permitted(const Segment& segment, const proc::Task& task, bool write) {
    if (task.euid == 0) {
        return true;
    }
    unsigned int shift = 0;
    if (task.euid == segment.uid) {
        shift = 6;
    } else if (task.egid == segment.gid) {
        shift = 3;
    }
    const auto kWanted = write ? 6U : 4U;
    return ((segment.mode >> shift) & kWanted) == kWanted;
}

base::Shared<mm::SharedPages> allocate_pages(unsigned long bytes) {
    base::Shared<mm::SharedPages> pages(new mm::SharedPages{});
    if (!pages.holds()) {
        return {};
    }
    const auto kCount = base::math::AlignUp(bytes, arch::page::kSize) / arch::page::kSize;
    pages->frames     = base::Own<unsigned long[]>(new unsigned long[kCount]{});
    pages->retire     = [](unsigned long physical) { mm::cow::ReleaseFrame(physical); };
    if (!pages->frames.holds()) {
        return {};
    }
    for (unsigned long index = 0; index < kCount; ++index) {
        const auto kFrame = mm::Pmm::self().allocate_page();
        if (kFrame == base::PhysAddr{}) {
            return {};
        }
        mm::Pmm::self().retain_frame(kFrame);
        base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kFrame.raw)), 0, arch::page::kSize);
        pages->frames[pages->count++] = kFrame.raw;
    }
    return pages;
}

}  // namespace

long long HandleShmget(const SyscallFrame& frame) {
    auto&      world  = ShmWorld::self();
    const auto kGuard = world.gate.guard();
    const auto kKey   = static_cast<std::uint32_t>(frame.rdi);
    const auto kFlags = frame.rdx;
    Segment**  vacant = nullptr;
    for (Segment*& segment : world.entries) {
        if (segment == nullptr) {
            vacant = &segment;
            continue;
        }
        if (kKey == 0 || segment->key != kKey) {
            continue;
        }
        if ((kFlags & (kIpcCreate | kIpcExclusive)) == (kIpcCreate | kIpcExclusive)) {
            return -kEexist;
        }
        if (frame.rsi > segment->bytes) {
            return -kEinval;
        }
        if (!permitted(*segment, *CurrentTask(), (kFlags & 0222ULL) != 0)) {
            return -kEperm;
        }
        return static_cast<long long>(segment->identity);
    }
    if (kKey != 0 && (kFlags & kIpcCreate) == 0) {
        return -kEnoent;
    }
    if (frame.rsi == 0 || frame.rsi > kSegmentBytesMax) {
        return -kEinval;
    }
    if (vacant == nullptr) {
        return -kEnomem;
    }
    base::Shared<mm::SharedPages> pages = allocate_pages(frame.rsi);
    if (!pages.holds()) {
        return -kEnomem;
    }
    auto* const kBorn = new Segment{
        .pages    = std::move(pages),
        .key      = kKey,
        .uid      = CurrentTask()->euid,
        .gid      = CurrentTask()->egid,
        .mode     = static_cast<std::uint32_t>(kFlags & 0777ULL),
        .creator  = static_cast<std::uint32_t>(CurrentTask()->tgid),
        .bytes    = frame.rsi,
        .identity = world.next_identity++,
    };
    if (kBorn == nullptr) {
        return -kEnomem;
    }
    *vacant = kBorn;
    return static_cast<long long>(kBorn->identity);
}

long long HandleShmat(const SyscallFrame& frame) {
    base::Shared<mm::SharedPages> pages;
    const proc::Task* const       kSelf = CurrentTask();
    if (!kSelf->vmas.holds() || !kSelf->space.holds() || (frame.rdx & ~(kReadOnly | kRound)) != 0) {
        return -kEinval;
    }
    {
        auto&                world    = ShmWorld::self();
        const auto           kGuard   = world.gate.guard();
        const Segment* const kSegment = world.find(frame.rdi);
        if (kSegment == nullptr) {
            return -kEinval;
        }
        if (!permitted(*kSegment, *kSelf, (frame.rdx & kReadOnly) == 0)) {
            return -kEperm;
        }
        pages = kSegment->pages;
    }
    const unsigned long kBytes = pages->count * arch::page::kSize;
    unsigned long       start  = frame.rsi;
    if ((frame.rdx & kRound) != 0) {
        start = base::math::AlignDown(start, arch::page::kSize);
    }
    if (start == 0) {
        const auto kSpot = kSelf->vmas->find_free_spot(kSelf->vmas->mmap_ceiling(), kBytes);
        if (!kSpot.ok()) {
            return -kEnomem;
        }
        start = kSpot.value();
    }
    if (start % arch::page::kSize != 0 || start < mm::kUserNullTop || start >= kUserAddressLimit ||
        kBytes > kUserAddressLimit - start) {
        return -kEinval;
    }
    mm::VmaRegion region{};
    region.start    = start;
    region.end      = start + kBytes;
    region.kind     = mm::VmaKind::kAnonymous;
    region.readable = true;
    region.writable = (frame.rdx & kReadOnly) == 0;
    region.shared   = base::Shared<mm::SharedAttachment>(
        new mm::SharedAttachment(std::move(pages), region.start, region.end));
    if (!region.shared.holds()) {
        return -kEnomem;
    }
    const auto kFiled = kSelf->vmas->register_range(region);
    if (!kFiled.ok()) {
        return -ErrnoFrom(kFiled.error());
    }
    return static_cast<long long>(start);
}

long long HandleShmdt(const SyscallFrame& frame) {
    const auto* const kSelf = CurrentTask();
    if (!kSelf->vmas.holds()) {
        return -kEinval;
    }
    const auto* const kRegion = kSelf->vmas->find(frame.rdi);
    if (kRegion == nullptr || !kRegion->shared.holds() || kRegion->shared->start != frame.rdi) {
        return -kEinval;
    }
    const auto kBytes = kRegion->shared->end - kRegion->shared->start;
    const auto kGone  = ReclaimUserSpan(*kSelf, frame.rdi, kBytes);
    return kGone.ok() ? 0 : -ErrnoFrom(kGone.error());
}

long long HandleShmctl(const SyscallFrame& frame) {
    auto&                world    = ShmWorld::self();
    const auto           kGuard   = world.gate.guard();
    const Segment* const kSegment = world.find(frame.rdi);
    if (kSegment == nullptr) {
        return -kEinval;
    }
    if (frame.rsi == 0) {
        if (CurrentTask()->euid != 0 && CurrentTask()->euid != kSegment->uid) {
            return -kEperm;
        }
        for (Segment*& slot : world.entries) {
            if (slot == kSegment) {
                slot = nullptr;
                break;
            }
        }
        delete kSegment;
        return 0;
    }
    if (frame.rsi != 2 || !permitted(*kSegment, *CurrentTask(), false)) {
        return -kEinval;
    }
    unsigned char record[112]{};
    base::WriteWord32(record, 0, kSegment->key);
    base::WriteWord32(record, 4, kSegment->uid);
    base::WriteWord32(record, 8, kSegment->gid);
    base::WriteWord32(record, 12, kSegment->uid);
    base::WriteWord32(record, 16, kSegment->gid);
    base::WriteWord32(record, 20, kSegment->mode);
    base::WriteWord64(record, 48, kSegment->bytes);
    base::WriteWord32(record, 80, kSegment->creator);
    base::WriteWord64(record, 88, kSegment->pages->attachments.load(std::memory_order_relaxed));
    return uaccess::CopyToUser(frame.rdx, record, sizeof(record)) ? 0 : -kEfault;
}

}  // namespace cinux::syscall
