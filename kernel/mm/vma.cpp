#include "kernel/mm/vma.hpp"

#include <utility>

#include "cinux/container/self_list.hpp"
#include "cinux/math.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/shared_pages.hpp"

namespace cinux::mm {

namespace {

using arch::page::kSize;

bool overlaps(const VmaRegion& existing, const VmaRegion& spec) {
    return existing.start < spec.end && spec.start < existing.end;
}

bool overlaps_span(const VmaRegion& existing, unsigned long start, unsigned long end) {
    return existing.start < end && start < existing.end;
}

bool covers(const VmaRegion& region, unsigned long address) {
    return region.start <= address && address < region.end;
}

class RegionBatch {
public:
    ~RegionBatch() {
        while (const VmaRegion* region = regions.pop_head()) {
            delete region;
        }
    }

    base::container::SelfList<VmaRegion> regions;
};

bool stage_protection(RegionBatch& batch, const VmaRegion& source, const VmaRegion& cut,
                      VmaProtection terms) {
    const unsigned long kCutStart = base::math::Max(cut.start, source.start);
    const unsigned long kCutEnd   = base::math::Min(cut.end, source.end);
    const unsigned long kCuts[]   = {source.start, kCutStart, kCutEnd, source.end};
    for (unsigned long index = 0; index < 3; ++index) {
        if (kCuts[index] == kCuts[index + 1]) {
            continue;
        }
        auto* const kPart = new VmaRegion(source);
        if (kPart == nullptr) {
            return false;
        }
        kPart->start = kCuts[index];
        kPart->end   = kCuts[index + 1];
        kPart->file_offset += kPart->start - source.start;
        if (index == 1) {
            kPart->readable   = terms.readable;
            kPart->writable   = terms.writable;
            kPart->executable = terms.executable;
        }
        batch.regions.push_back(*kPart);
    }
    return true;
}

}  // namespace

base::Result<void> VmaStore::register_range(const VmaRegion& spec) {
    if (spec.start >= spec.end) {
        return base::KernelError::kInvalidArgument;
    }
    if (!heap_recorded_ && heap_.start < heap_.end && overlaps(heap_, spec)) {
        return base::KernelError::kAlreadyExists;
    }
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (overlaps(*scan, spec)) {
            return base::KernelError::kAlreadyExists;
        }
    }
    auto* const kBorn = new VmaRegion{spec};
    if (kBorn == nullptr) {
        return base::KernelError::kOutOfMemory;
    }
    if (kBorn->kind == VmaKind::kStack && kBorn->stack_floor == 0) {
        kBorn->stack_floor = kBorn->start + kSize;
    }
    regions_.push_back(*kBorn);
    return base::Result<void>{};
}

const VmaRegion* VmaStore::find(unsigned long address) const {
    if (!heap_recorded_ && covers(heap_, address)) {
        return &heap_;
    }
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (covers(*scan, address)) {
            return scan;
        }
    }
    return nullptr;
}

bool VmaStore::stack_contains(unsigned long start, unsigned long bytes) const {
    if (bytes == 0 || bytes > ~0UL - start) {
        return false;
    }
    const auto kEnd = start + bytes;
    for (unsigned long cursor = start; cursor < kEnd;) {
        const VmaRegion* const kRegion = find(cursor);
        if (kRegion == nullptr || kRegion->kind != VmaKind::kStack || !kRegion->readable ||
            !kRegion->writable || cursor < kRegion->stack_floor) {
            return false;
        }
        cursor = base::math::Min(kRegion->end, kEnd);
    }
    return true;
}

VmaStore* VmaStore::clone() const {
    base::Own<VmaStore> copy(new VmaStore{});
    if (!copy.holds()) {
        return nullptr;
    }
    copy->heap_          = heap_;
    copy->heap_max_      = heap_max_;
    copy->heap_recorded_ = heap_recorded_;
    copy->mmap_ceiling_  = mmap_ceiling_;
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        VmaRegion region = *scan;
        if (scan->shared.holds()) {
            base::Shared<SharedAttachment> attachment;
            for (const VmaRegion* born = copy->regions_.head(); born != nullptr;
                 born                  = copy->regions_.after(*born)) {
                if (born->shared.holds() && born->shared->start == scan->shared->start) {
                    attachment = born->shared;
                    break;
                }
            }
            if (!attachment.holds()) {
                attachment = base::Shared<SharedAttachment>(new SharedAttachment(
                    scan->shared->pages, scan->shared->start, scan->shared->end));
                if (!attachment.holds()) {
                    return nullptr;
                }
            }
            region.shared = std::move(attachment);
        }
        const base::Result<void> kFiled = copy->register_range(region);
        if (!kFiled.ok()) {
            return nullptr;
        }
    }
    return copy.release();
}

void VmaStore::teardown() {
    while (const VmaRegion* const kRegion = regions_.pop_head()) {
        delete kRegion;
    }
}

void VmaStore::seed_heap(unsigned long base, unsigned long max) {
    heap_.start       = base;
    heap_.end         = base;
    heap_.kind        = VmaKind::kAnonymous;
    heap_.readable    = true;
    heap_.writable    = true;
    heap_.executable  = false;
    heap_.backing     = {};
    heap_.file_offset = 0;
    heap_max_         = max;
    heap_recorded_    = false;
}

base::Result<unsigned long> VmaStore::move_heap_end(unsigned long target) {
    if (heap_.start == 0 && heap_.end == 0) {
        return base::KernelError::kNotFound;
    }
    if (target == 0) {
        return heap_.end;
    }
    const unsigned long kWanted =
        cinux::base::math::Clamp(cinux::base::math::AlignUp(target, kSize), heap_.start, heap_max_);
    if (kWanted > heap_.end) {
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (overlaps_span(*scan, heap_.end, kWanted)) {
                return base::KernelError::kAlreadyExists;
            }
        }
    }
    if (heap_recorded_ && kWanted > heap_.end) {
        VmaRegion extension{};
        extension.start    = heap_.end;
        extension.end      = kWanted;
        extension.kind     = VmaKind::kAnonymous;
        extension.readable = true;
        extension.writable = true;
        const auto kAdded  = register_range(extension);
        if (!kAdded.ok()) {
            return kAdded.error();
        }
    } else if (heap_recorded_ && kWanted < heap_.end) {
        const auto kRemoved = unregister_range(kWanted, heap_.end);
        if (!kRemoved.ok()) {
            return kRemoved.error();
        }
    }
    heap_.end = kWanted;
    return kWanted;
}

base::Result<unsigned long> VmaStore::find_free_spot(unsigned long ceiling,
                                                     unsigned long bytes) const {
    if (bytes == 0 || ceiling < bytes) {
        return base::KernelError::kOutOfMemory;
    }
    unsigned long spot_end = ceiling;
    for (bool moved = true; moved;) {
        moved = false;
        if (!heap_recorded_ && heap_.start < spot_end && heap_.end > spot_end - bytes) {
            spot_end = heap_.start;
            moved    = true;
        }
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (scan->start < spot_end && scan->end > spot_end - bytes) {
                spot_end = scan->start;
                moved    = true;
            }
        }
    }
    if (spot_end < bytes) {
        return base::KernelError::kOutOfMemory;
    }
    return spot_end - bytes;
}

base::Result<void> VmaStore::unregister_range(unsigned long start, unsigned long end) {
    if (start >= end) {
        return base::KernelError::kInvalidArgument;
    }
    if (!heap_recorded_ && heap_.start < heap_.end && overlaps_span(heap_, start, end)) {
        if (end < heap_.end) {
            auto* const kTail = new VmaRegion{};
            if (kTail == nullptr) {
                return base::KernelError::kOutOfMemory;
            }
            kTail->start      = end;
            kTail->end        = heap_.end;
            kTail->kind       = VmaKind::kAnonymous;
            kTail->readable   = heap_.readable;
            kTail->writable   = heap_.writable;
            kTail->executable = heap_.executable;
            regions_.push_back(*kTail);
        }
        if (start <= heap_.start) {
            heap_.end = heap_.start;
        } else {
            heap_.end = start;
        }
    }
    for (VmaRegion* scan = regions_.head(); scan != nullptr;) {
        VmaRegion* const kNext = regions_.after(*scan);
        if (overlaps_span(*scan, start, end)) {
            const bool kKeepsHead = scan->start < start;
            const bool kKeepsTail = scan->end > end;
            if (kKeepsHead && kKeepsTail) {
                auto* const kTail = new VmaRegion(*scan);
                if (kTail == nullptr) {
                    return base::KernelError::kOutOfMemory;
                }
                kTail->start       = end;
                kTail->file_offset = scan->file_offset + (end - scan->start);
                scan->end          = start;
                regions_.push_back(*kTail);
            } else if (kKeepsHead) {
                scan->end = start;
            } else if (kKeepsTail) {
                const unsigned long kOldStart = scan->start;
                scan->start                   = end;
                scan->file_offset += end - kOldStart;
            } else {
                regions_.remove(*scan);
                delete scan;
            }
        }
        scan = kNext;
    }
    return base::Result<void>{};
}

base::Result<void> VmaStore::protect(unsigned long start, unsigned long end, VmaProtection terms) {
    if (start >= end || !span_known(start, end)) {
        return base::KernelError::kInvalidArgument;
    }
    RegionBatch batch;
    VmaRegion   cut{};
    cut.start = start;
    cut.end   = end;
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (overlaps_span(*scan, start, end) && !stage_protection(batch, *scan, cut, terms)) {
            return base::KernelError::kOutOfMemory;
        }
    }
    const bool kHeapCovered =
        !heap_recorded_ && heap_.start < heap_.end && overlaps_span(heap_, start, end);
    const bool kWholeHeap = kHeapCovered && start <= heap_.start && heap_.end <= end;
    if (kHeapCovered && !kWholeHeap && !stage_protection(batch, heap_, cut, terms)) {
        return base::KernelError::kOutOfMemory;
    }
    for (VmaRegion* scan = regions_.head(); scan != nullptr;) {
        VmaRegion* const kNext = regions_.after(*scan);
        if (overlaps_span(*scan, start, end)) {
            regions_.remove(*scan);
            delete scan;
        }
        scan = kNext;
    }
    if (kWholeHeap) {
        heap_.readable   = terms.readable;
        heap_.writable   = terms.writable;
        heap_.executable = terms.executable;
    } else if (kHeapCovered) {
        heap_recorded_ = true;
    }
    while (VmaRegion* region = batch.regions.pop_head()) {
        regions_.push_back(*region);
    }
    return base::Result<void>{};
}

bool VmaStore::span_known(unsigned long start, unsigned long end) const {
    unsigned long spot = start;
    while (spot < end) {
        unsigned long reach = spot;
        if (!heap_recorded_ && heap_.start <= spot && spot < heap_.end) {
            reach = heap_.end;
        }
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (scan->start <= spot && spot < scan->end) {
                reach = cinux::base::math::Max(reach, scan->end);
            }
        }
        if (reach <= spot) {
            return false;
        }
        spot = reach;
    }
    return true;
}

unsigned long VmaStore::max_end() const {
    unsigned long highest = heap_.end;
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        highest = cinux::base::math::Max(highest, scan->end);
    }
    return highest;
}

}  // namespace cinux::mm
