#include "kernel/mm/vma.hpp"

#include "cinux/math.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"

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

bool inside(unsigned long outer_start, unsigned long outer_end, unsigned long inner_start,
            unsigned long inner_end) {
    return outer_start <= inner_start && inner_end <= outer_end;
}

}  // namespace

base::Result<void> VmaStore::register_range(const VmaRegion& spec) {
    if (spec.start >= spec.end) {
        return base::KernelError::kInvalidArgument;
    }
    if (heap_.start < heap_.end && overlaps(heap_, spec)) {
        return base::KernelError::kAlreadyExists;
    }
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (overlaps(*scan, spec)) {
            return base::KernelError::kAlreadyExists;
        }
    }
    auto* const kBorn = new VmaRegion{spec};
    regions_.push_back(*kBorn);
    return base::Result<void>{};
}

const VmaRegion* VmaStore::find(unsigned long address) const {
    if (covers(heap_, address)) {
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
    const VmaRegion* const kRegion = find(start);
    return bytes != 0 && kRegion != nullptr && kRegion->kind == VmaKind::kStack &&
           kRegion->readable && kRegion->writable && start - kRegion->start >= kSize &&
           bytes <= kRegion->end - start;
}

VmaStore* VmaStore::clone() const {
    base::Own<VmaStore> copy(new VmaStore{});
    if (!copy.holds()) {
        return nullptr;
    }
    copy->heap_         = heap_;
    copy->heap_max_     = heap_max_;
    copy->mmap_ceiling_ = mmap_ceiling_;
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        const base::Result<void> kFiled = copy->register_range(*scan);
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
    if (kWanted < heap_.end) {
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (overlaps_span(*scan, kWanted, heap_.end)) {
                return base::KernelError::kAlreadyExists;
            }
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
        if (heap_.start < spot_end && heap_.end > spot_end - bytes) {
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

bool VmaStore::movable_within(unsigned long start, unsigned long end) const {
    if (heap_.start < heap_.end && overlaps_span(heap_, start, end) &&
        !inside(start, end, heap_.start, heap_.end)) {
        return false;
    }
    for (const VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (overlaps_span(*scan, start, end) && !inside(start, end, scan->start, scan->end)) {
            return false;
        }
    }
    return true;
}

base::Result<void> VmaStore::unregister_range(unsigned long start, unsigned long end) {
    if (start >= end || !movable_within(start, end)) {
        return base::KernelError::kInvalidArgument;
    }
    if (heap_.start < heap_.end && inside(start, end, heap_.start, heap_.end)) {
        heap_.end = heap_.start;
    }
    for (VmaRegion* scan = regions_.head(); scan != nullptr;) {
        VmaRegion* const kNext = regions_.after(*scan);
        if (inside(start, end, scan->start, scan->end)) {
            regions_.remove(*scan);
            delete scan;
        }
        scan = kNext;
    }
    return base::Result<void>{};
}

base::Result<void> VmaStore::protect(unsigned long start, unsigned long end, VmaProtection terms) {
    if (start >= end || !span_known(start, end)) {
        return base::KernelError::kInvalidArgument;
    }
    if (heap_.start < heap_.end && (inside(heap_.start, heap_.end, start, end) ||
                                    inside(start, end, heap_.start, heap_.end))) {
        heap_.readable   = terms.readable;
        heap_.writable   = terms.writable;
        heap_.executable = terms.executable;
    }
    for (VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (inside(scan->start, scan->end, start, end) ||
            inside(start, end, scan->start, scan->end)) {
            scan->readable   = terms.readable;
            scan->writable   = terms.writable;
            scan->executable = terms.executable;
        }
    }
    return base::Result<void>{};
}

bool VmaStore::span_known(unsigned long start, unsigned long end) const {
    unsigned long spot = start;
    while (spot < end) {
        unsigned long reach = spot;
        if (heap_.start <= spot && spot < heap_.end) {
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
