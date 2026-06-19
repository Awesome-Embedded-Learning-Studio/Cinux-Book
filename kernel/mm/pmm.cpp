/**
 * @file kernel/mm/pmm.cpp
 * @brief Physical Memory Manager -- buddy-system allocator implementation
 */

#include "kernel/mm/pmm.hpp"

#include <stddef.h>

#include "kernel/lib/kprintf.hpp"

namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t LOW_MEM_BOUNDARY = 0x100000;  // 1 MB
constexpr uint64_t KERNEL_VMA       = 0xFFFFFFFF80000000ULL;

// ============================================================
// Linker symbols
// ============================================================

extern "C" {
extern char __kernel_stack_top;
}

// ============================================================
// Global instance
// ============================================================

PMM g_pmm;

// ============================================================
// parse_memory_map
// ============================================================

uint32_t parse_memory_map(const BootInfo& info, MemoryRegion* regions, uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& entry = info.mmap[i];
        if (entry.type != 1)
            continue;

        uint64_t base   = entry.base;
        uint64_t length = entry.length;

        // Filter: everything below 1 MB is reserved
        if (base < LOW_MEM_BOUNDARY) {
            if (base + length <= LOW_MEM_BOUNDARY)
                continue;
            length -= LOW_MEM_BOUNDARY - base;
            base = LOW_MEM_BOUNDARY;
        }

        // Align base up, length down to 4 KB
        uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned_base - base);
        length &= ~(PAGE_SIZE - 1);

        if (length < PAGE_SIZE)
            continue;
        regions[count++] = {aligned_base, length};
    }

    return count;
}

// ============================================================
// PMM::init
// ============================================================

void PMM::init(const BootInfo& info) {
    // Step 1: Extract usable memory regions
    MemoryRegion regions[32];
    uint32_t     region_count = parse_memory_map(info, regions, 32);

    // Step 2: Determine highest physical address -> total pages
    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (end > max_addr)
            max_addr = end;
    }
    highest_page_ = max_addr / PAGE_SIZE;
    total_pages_  = highest_page_;

    // Step 3: Place the per-page order array (1 byte/page) after the kernel
    // stack, page-aligned -- where the old bitmap lived.
    uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
    uintptr_t os_virt        = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    order_storage_           = reinterpret_cast<uint8_t*>(os_virt);

    // Step 4: Place the buddy's per-order free bitmaps right after the order
    // array (page-aligned).  The bitmaps track free blocks without writing into
    // the free pages themselves (GOTCHA #14 -- nested-KVM safe).
    uint64_t  order_bytes = total_pages_;
    uintptr_t bs_virt     = (os_virt + order_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bitmap_storage_       = reinterpret_cast<uint8_t*>(bs_virt);
    uint64_t bm_bytes     = BuddyAllocator::bitmap_bytes(total_pages_);

    // Step 5: Initialise the buddy over page indices [0, total_pages).
    buddy_.init(0, total_pages_, order_storage_, bitmap_storage_);

    // Step 6: Mark usable RAM free, EXCLUDING the permanently-reserved span
    // [kernel_phys_base, metadata_end) -- the kernel image, stack, order array
    // and the free bitmaps themselves.  Pages never marked free stay invisible
    // to the allocator (free() is a no-op on them), so the kernel's own pages
    // are never handed out (the F2-M7 wiring trampling root cause).
    // The order array and the per-order bitmaps sit contiguously after the
    // kernel image (order first, then bitmaps), so the bitmap tail covers both
    // metadata regions.  Exclude the whole span [kernel_phys_base, bitmap tail)
    // from the free pool.
    uint64_t bitmap_phys  = bs_virt - KERNEL_VMA;
    uint64_t bitmap_pages = (bm_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t used_start   = info.kernel_phys_base;
    uint64_t used_end     = bitmap_phys + bitmap_pages * PAGE_SIZE;

    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t seg_start = regions[i].base;
        uint64_t seg_end   = regions[i].base + regions[i].length;
        if (used_end <= seg_start || used_start >= seg_end) {
            buddy_.mark_free_region(seg_start / PAGE_SIZE, (seg_end - seg_start) / PAGE_SIZE);
        } else {
            if (used_start > seg_start)
                buddy_.mark_free_region(seg_start / PAGE_SIZE,
                                        (used_start - seg_start) / PAGE_SIZE);
            if (used_end < seg_end)
                buddy_.mark_free_region(used_end / PAGE_SIZE, (seg_end - used_end) / PAGE_SIZE);
        }
    }

    // Step 7: Print statistics
    uint64_t total_mb = total_pages_ * PAGE_SIZE / (1024 * 1024);
    uint64_t free_mb  = buddy_.free_pages() * PAGE_SIZE / (1024 * 1024);
    cinux::lib::kprintf("[PMM] Total: %luMB, Free: %luMB\n", total_mb, free_mb);
}

// ============================================================
// PMM::alloc_page_locked / free_page_locked (no lock)
// ============================================================

uint64_t PMM::alloc_page_locked() {
    uint64_t page = buddy_.alloc_order(0);
    if (page == BuddyAllocator::kInvalidPage)
        return 0;
    return page * PAGE_SIZE;
}

void PMM::free_page_locked(uint64_t phys) {
    if (phys == 0)
        return;
    buddy_.free(phys / PAGE_SIZE);
}

// ============================================================
// PMM::alloc_page / free_page (public, locked)
// ============================================================

uint64_t PMM::alloc_page() {
    auto g = lock_.guard();
    (void)g;
    return alloc_page_locked();
}

void PMM::free_page(uint64_t phys) {
    auto g = lock_.guard();
    (void)g;
    free_page_locked(phys);
}

// ============================================================
// PMM::alloc_pages / free_pages (public, locked)
// ============================================================

uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 0)
        return 0;

    // Round up to the smallest buddy order holding >= count pages.
    int      order = 0;
    uint64_t n     = 1;
    while (n < count) {
        n <<= 1;
        order++;
    }
    if (order > BuddyAllocator::kMaxOrder)
        return 0;

    auto g = lock_.guard();
    (void)g;
    uint64_t page = buddy_.alloc_order(order);
    if (page == BuddyAllocator::kInvalidPage)
        return 0;
    return page * PAGE_SIZE;
}

void PMM::free_pages(uint64_t phys, uint64_t count) {
    // The buddy records each head's order authoritatively, so @p count is not
    // needed: freeing the head returns the whole power-of-two block.
    (void)count;
    free_page(phys);
}

// ============================================================
// PMM statistics
// ============================================================

uint64_t PMM::free_page_count() const {
    return buddy_.free_pages();
}
uint64_t PMM::total_page_count() const {
    return total_pages_;
}

}  // namespace cinux::mm
