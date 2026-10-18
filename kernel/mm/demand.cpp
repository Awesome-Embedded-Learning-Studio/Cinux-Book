/**
 * @file    demand.cpp
 * @brief   One fault, one promise, one frame: the fill itself.
 *
 * @author  Charliechen114514
 * @date    2026-10-15
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#include "kernel/mm/demand.hpp"

#include "cinux/addr.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/isr.hpp"  // NOLINT(misc-include-cleaner) fault-code words shared with the reporter
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm::demand {

namespace {

using arch::page::kSize;
using arch::page::Entry;

/// @brief Reads one page of a file-backed range into a fresh frame's
///        direct-map alias; a short read at the file's tail is the bss
///        answer, so only a refusal fails.
[[nodiscard]] bool read_backing_page(const VmaRegion& region, unsigned long page,
                                     const base::PhysAddr& frame) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const              kSink = reinterpret_cast<void*>(DirectMapVirt(frame.raw));
    const unsigned long long kFileAt =
        region.file_offset + static_cast<unsigned long long>(page - region.start);
    return region.file_ops->read(region.file_self, region.backing, kFileAt, kSink, kSize).ok();
}

}  // namespace

bool FillPage(const AddressSpace& space, const VmaStore& ranges, unsigned long long address,
              unsigned long long error_code) {
    if ((error_code & cinux::arch::isr::kFaultProtection) != 0 || address >= kUserTop) {
        return false;
    }
    const VmaRegion* const kRegion = ranges.find(address);
    if (kRegion == nullptr) {
        return false;
    }
    const unsigned long kPage = cinux::base::math::AlignDown(address, kSize);
    if (kRegion->kind == VmaKind::kStack && kPage == kRegion->start) {
        return false;
    }
    KernelTables       world;
    const walk::SlotAt kSlot = walk::FindSlot(world, space.root(), kPage);
    if (kSlot.entry != nullptr && kSlot.entry->has(cinux::arch::page::kPresent)) {
        return false;
    }
    const base::PhysAddr kFrame = Pmm::self().allocate_page();
    if (kFrame == base::PhysAddr{}) {
        return false;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kBytes = cinux::base::PtrAt<unsigned char>(DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kBytes, 0, kSize);
    if (kRegion->kind == VmaKind::kFile &&
        (kRegion->file_ops == nullptr || !read_backing_page(*kRegion, kPage, kFrame))) {
        Pmm::self().free_page(kFrame);
        return false;
    }
    Entry bits = cinux::arch::page::kUser;
    if (kRegion->writable) {
        bits = bits | cinux::arch::page::kWritable;
    }
    if (!space.map_user(kPage, kFrame.raw, bits)) {
        Pmm::self().free_page(kFrame);
        return false;
    }
    cinux::arch::InvalidatePage(kPage);
    return true;
}

}  // namespace cinux::mm::demand
