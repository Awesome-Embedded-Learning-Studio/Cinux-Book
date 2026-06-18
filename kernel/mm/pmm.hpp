/**
 * @file kernel/mm/pmm.hpp
 * @brief Physical Memory Manager -- buddy-system allocator
 *
 * Manages physical page allocation using a buddy allocator (power-of-two free
 * lists) over all usable RAM.  The per-page order metadata (1 byte/page) sits
 * where the old bitmap did, immediately after the kernel image + stack.
 * Supports single and contiguous multi-page allocation; multi-page allocs
 * round up to the next power of two (see alloc_pages).
 */

#pragma once

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/mm/buddy.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::mm {

/// A usable physical memory region extracted from the E820 map.
struct MemoryRegion {
    uint64_t base;
    uint64_t length;
};

/**
 * @brief Extract usable memory regions from the BIOS memory map
 *
 * Filters for type-1 (usable) entries, removes anything below 1 MB,
 * and aligns each region to 4 KB boundaries.
 *
 * @param info          Boot information from the bootloader
 * @param regions       Output array for extracted regions
 * @param max_regions   Capacity of the regions array
 * @return Number of regions written
 */
uint32_t parse_memory_map(const BootInfo& info, MemoryRegion* regions, uint32_t max_regions);

/**
 * @brief Physical Memory Manager backed by a BuddyAllocator
 *
 * The per-page order array (1 byte/page) is placed at __kernel_stack_top
 * (virtual), page-aligned.  Public alloc/free take the spinlock; the _locked
 * variants rely on caller-provided exclusion (interrupts disabled, e.g. the
 * page-fault path), matching the BuddyAllocator's own "caller holds exclusion"
 * contract.
 */
class PMM {
public:
    /** Initialise from the bootloader-provided memory map. */
    void init(const BootInfo& info);

    /** Allocate a single 4 KB page.  Returns physical address, 0 on OOM. */
    uint64_t alloc_page();

    /** Free a single page (no-op if phys is 0 or already free). */
    void free_page(uint64_t phys);

    /** Allocate @p count contiguous pages.  Returns base phys addr, 0 on OOM.
     *  The block is rounded up to the next power of two (buddy order); free it
     *  via free_pages() with the same base -- the recorded order is authoritative. */
    uint64_t alloc_pages(uint64_t count);

    /** Free the block whose head is @p phys (@p count is ignored -- the buddy's
     *  recorded order drives coalescing).  No-op if @p phys is not an allocated head. */
    void free_pages(uint64_t phys, uint64_t count);

    /** Current number of free pages. */
    uint64_t free_page_count() const;

    /** Total number of pages managed. */
    uint64_t total_page_count() const;

    /**
     * @brief Lock-free page allocation (caller must guarantee exclusion)
     *
     * Does NOT acquire the internal spinlock.  Safe only from contexts
     * where interrupts are disabled and no concurrent PMM access is
     * possible (e.g. page fault handler under Interrupt gate).
     */
    uint64_t alloc_page_locked();

    /** Lock-free page free (caller must guarantee exclusion). */
    void free_page_locked(uint64_t phys);

private:
    cinux::proc::Spinlock lock_;
    BuddyAllocator        buddy_;
    uint8_t*              order_storage_{};  ///< 1 byte/page, @ __kernel_stack_top
    uint8_t*              bitmap_storage_{};  ///< per-order free bitmaps, after order_storage
    uint64_t              total_pages_{};
    uint64_t              highest_page_{};
};

/// Global PMM instance.
extern PMM g_pmm;

}  // namespace cinux::mm
