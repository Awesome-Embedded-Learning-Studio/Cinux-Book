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

    // F-QA Q4b-1 (DEBT-003): per-page pte_count = how many user PTEs map this
    // physical page. fork CoW sharing and file/shm mapping call inc; teardown
    // / munmap / CoW fault call dec_and_test. This counter NEVER frees on its
    // own -- it only tracks PTE mappings (batch 3 split: was the unified
    // mapcount that also freed). Freeing is driven by refcount below.
    void    pte_count_inc(uint64_t phys);
    void    pte_count_dec(uint64_t phys);  ///< pure PTE -1 (never frees)
    int16_t pte_count_load(uint64_t phys) const;

    // pte_count_dec_and_test: drop one PTE ref; when pte_count reaches 0 also
    // drop one ownership refcount (which frees the page when THAT reaches 0).
    // Returns true iff the page was freed (both counters hit 0).  Mirrors the
    // old unified mapcount_dec_and_test contract so the 7 teardown callers stay
    // mechanical -- the difference is that a page still owned by the page cache
    // (CachePhysRef) or a live shmem segment keeps refcount > 0 and survives
    // teardown even with pte_count == 0 (the lto_plugin corruption root cause
    // f06ea6b prevented at the type layer instead of by a phantom pte_count+1).
    bool pte_count_dec_and_test(uint64_t phys);

    // B3 defect C: deferred-free variant.  Like pte_count_dec_and_test but
    // does NOT free -- returns true when the page would be freed (pte_count
    // hit 0, refcount hit 0, audit passed).  Caller frees later via free_page()
    // after a cross-core TLB shootdown.  Audit (free-vs-pte / free-vs-cache)
    // still runs at dec time so a bad free is caught instantly.
    bool pte_count_dec_and_test_no_free(uint64_t phys);

    // Per-page refcount = ownership refs. alloc_page sets 1 (the lone-owner
    // baseline, which doubles as page-cache ownership via CachePhysRef); shm
    // attach adds an inc. Last ref frees via buddy. pte_count only tracks
    // PTE mappings and never frees on its own.
    void    refcount_inc(uint64_t phys);
    bool    refcount_dec_and_test(uint64_t phys);  ///< true => reached 0, page freed
    bool    refcount_dec_and_test_no_free(uint64_t phys);  ///< B3: true => would free, NOT freed
    int16_t refcount_load(uint64_t phys) const;

private:
    // Lock-free buddy access (PRIVATE).  The buddy is NOT thread-safe; these
    // bypass the PMM spinlock and are callable ONLY from the internal
    // alloc_page / free_page (which acquire lock_).  They used to be public,
    // which let the page-fault path (vmm walk_level, handle_pf file/anon) grab
    // pages without the spinlock -- on SMP that raced handle_cow_fault's locked
    // alloc and corrupted the buddy (clear_bit(kInvalidPage) #GP at
    // alloc_order+0x9c).  Keeping them private forces every external caller
    // through the locked public API by construction.
    uint64_t alloc_page_locked();
    void     free_page_locked(uint64_t phys);

    cinux::proc::Spinlock lock_;
    BuddyAllocator        buddy_;
    uint8_t*              order_storage_{};      ///< 1 byte/page, @ __kernel_stack_top
    uint8_t*              bitmap_storage_{};     ///< per-order free bitmaps, after order_storage
    int16_t*              pte_count_storage_{};  ///< Q4b-1: 2 bytes/page, after bitmap_storage
    int16_t*              refcount_storage_{};   ///< batch 3: ownership refs, after pte_count
    uint64_t              total_pages_{};
    uint64_t              highest_page_{};
};

/// Global PMM instance.
extern PMM g_pmm;

}  // namespace cinux::mm
