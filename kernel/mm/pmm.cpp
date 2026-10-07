#include "kernel/mm/pmm.hpp"

#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/math.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"  // NOLINT(misc-include-cleaner) IrqGuard is used below
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/mm/pmm_config.hpp"

namespace cinux::mm {

using cinux::base::PhysAddr;

bool Pmm::init(const cinux::boot::BootInfo& info) {
    bitmap_.init(words_, kPmmTotalPages);
    bitmap_.set_all();
    managed_pages_ = 0;
    for (uint32_t index = 0; index < info.e820_count; ++index) {
        if (info.e820[index].type != cinux::boot::kE820Usable) {
            continue;
        }
        auto const kEntryBase = static_cast<unsigned long>(info.e820[index].base);
        auto const kEntryTop  = kEntryBase + static_cast<unsigned long>(info.e820[index].length);
        auto const kLo        = kEntryBase > kLowMemoryTop ? kEntryBase : kLowMemoryTop;
        auto const kHi        = kEntryTop < kPmmMaxPhys ? kEntryTop : kPmmMaxPhys;
        if (kLo >= kHi) {
            continue;
        }
        unsigned long const kPageCount =
            cinux::base::math::Span(kLo, kHi, cinux::arch::page::kSize);
        bitmap_.clear_range(cinux::base::math::Floor(kLo, cinux::arch::page::kSize), kPageCount);
        managed_pages_ += kPageCount;
    }
    PhysAddr const      kKernelBase{static_cast<unsigned long>(info.kernel_paddr)};
    unsigned long const kKernelBytes{static_cast<unsigned long>(info.kernel_mem_size)};
    unsigned long const kKernelCount = cinux::base::math::Span(
        kKernelBase.raw, kKernelBase.offset(kKernelBytes).raw, cinux::arch::page::kSize);
    bitmap_.set_range(cinux::base::math::Floor(kKernelBase.raw, cinux::arch::page::kSize),
                      kKernelCount);
    return true;
}

PhysAddr Pmm::allocate_pages(int order) {
    if (order < 0 || order > kMaxOrder) {
        return PhysAddr{};
    }
    return allocate_run(1UL << order);
}

void Pmm::free_pages(PhysAddr phys, int order) {
    if (phys == PhysAddr{}) {
        return;
    }
    bitmap_.clear_range(cinux::base::math::Floor(phys.raw, cinux::arch::page::kSize), 1UL << order);
}

unsigned long Pmm::free_page_count() const {
    return bitmap_.clear_count();
}

unsigned long Pmm::managed_page_count() const {
    return managed_pages_;
}

PhysAddr Pmm::allocate_run(unsigned long page_count) {
    unsigned long const kBasePage = bitmap_.find_clear_run(page_count);
    if (kBasePage == cinux::base::bit::kInvalidIndex) {
        return PhysAddr{};
    }
    bitmap_.set_range(kBasePage, page_count);
    return PhysAddr{kBasePage * cinux::arch::page::kSize};
}

}  // namespace cinux::mm
