/**
 * @file kernel/mm/pmm.cpp
 * @brief Physical Memory Manager -- buddy-system allocator implementation
 */

#include "kernel/mm/pmm.hpp"

#include <stddef.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/page_cache.hpp"  // free-vs-cache audit (always-on)

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

    // Step 4b (F-QA Q4b-1 / DEBT-003): place the per-page pte_count array
    // (2 bytes/page) right after the free bitmaps, page-aligned. Tracks how
    // many PTEs reference each physical page so a CoW-shared page is not freed
    // while still mapped elsewhere (fork+exec UAF).
    uintptr_t mc_virt = (bs_virt + bm_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pte_count_storage_ = reinterpret_cast<int16_t*>(mc_virt);
    uint64_t mc_bytes = total_pages_ * sizeof(int16_t);
    for (uint64_t i = 0; i < total_pages_; ++i) {
        pte_count_storage_[i] = 0;
    }

    // Step 4c (C refactor batch 3): place the per-page refcount array
    // (2 bytes/page) right after pte_count_storage_, page-aligned.  Tracks
    // ownership refs (alloc baseline + cache/shm owners); the last ref frees
    // the page.  Distinct from pte_count (mapping count) -- mirrors Linux's
    // page->_refcount vs _mapcount split, and is the dimension that actually
    // drives free (pte_count above only tracks PTE mappings).
    uintptr_t rc_virt = (mc_virt + mc_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    refcount_storage_ = reinterpret_cast<int16_t*>(rc_virt);
    uint64_t rc_bytes = total_pages_ * sizeof(int16_t);
    for (uint64_t i = 0; i < total_pages_; ++i) {
        refcount_storage_[i] = 0;
    }

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
    uint64_t rc_phys    = rc_virt - KERNEL_VMA;
    uint64_t rc_pages   = (rc_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t used_start = info.kernel_phys_base;
    uint64_t used_end   = rc_phys + rc_pages * PAGE_SIZE;  // covers order+bitmap+pte_count+refcount

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
    // batch 3 split: refcount=1 (ownership baseline; the caller is the lone
    // owner until a PTE mapping / cache ref is added), pte_count=0 (no user
    // PTE maps it yet).  Was a single mapcount=1 before the split.
    __atomic_store_n(&refcount_storage_[page], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&pte_count_storage_[page], 0, __ATOMIC_RELAXED);
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
    return alloc_page_locked();
}

void PMM::free_page(uint64_t phys) {
    auto g = lock_.guard();
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
    uint64_t page = buddy_.alloc_order(order);
    if (page == BuddyAllocator::kInvalidPage)
        return 0;
    // batch 3 split: each page in the block starts with refcount=1 (ownership)
    // and pte_count=0 (no mappings).
    uint64_t n_pages = static_cast<uint64_t>(1) << order;
    for (uint64_t i = 0; i < n_pages; ++i) {
        __atomic_store_n(&refcount_storage_[page + i], 1, __ATOMIC_RELAXED);
        __atomic_store_n(&pte_count_storage_[page + i], 0, __ATOMIC_RELAXED);
    }
    return page * PAGE_SIZE;
}

void PMM::free_pages(uint64_t phys, [[maybe_unused]] uint64_t count) {
    // The buddy records each head's order authoritatively, so @p count is not
    // needed: freeing the head returns the whole power-of-two block.
    free_page(phys);
}

// ============================================================
// PMM::pte_count_* (F-QA Q4b-1 / DEBT-003 CoW page reference counting)
// ============================================================

void PMM::pte_count_inc(uint64_t phys) {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return;  // unmanaged (device/IoPhys)
    }
    // ACQ_REL: fork (parent CPU), clear_user_mappings (child CPU), and CoW
    // fault (faulting CPU) all read/write the same pte_count cross-CPU.
    __atomic_add_fetch(&pte_count_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL);
}

void PMM::pte_count_dec(uint64_t phys) {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return;
    }
    // Pure PTE -1; never frees.  Free is driven by refcount_dec_and_test.
    __atomic_sub_fetch(&pte_count_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL);
}

bool PMM::pte_count_dec_and_test(uint64_t phys) {
    // Drop one PTE ref.  When pte_count reaches 0, also drop the ownership
    // refcount that alloc_page set (the "address space owns this page" ref);
    // if THAT reaches 0 the page goes back to the buddy.  A page still owned
    // by the page cache (CachePhysRef) or a live shmem segment keeps refcount
    // > 0 after the drop and survives teardown -- the type-level guarantee
    // f06ea6b's phantom pte_count+1 used to paper over.  Callers must NOT
    // free_page() on a true return: the page is already freed here.
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return false;  // unmanaged (device/IoPhys)
    }
    if (__atomic_sub_fetch(&pte_count_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL) != 0) {
        return false;  // other PTEs still map it
    }
    return refcount_dec_and_test(phys);  // last PTE gone -> drop ownership ref (maybe free)
}

bool PMM::pte_count_dec_and_test_no_free(uint64_t phys) {
    // Same as pte_count_dec_and_test but does NOT free -- the caller (drain
    // kthread path) frees via free_page() after a cross-core TLB shootdown.
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return false;
    }
    if (__atomic_sub_fetch(&pte_count_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL) != 0) {
        return false;
    }
    return refcount_dec_and_test_no_free(phys);
}

int16_t PMM::pte_count_load(uint64_t phys) const {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return 0;
    }
    return __atomic_load_n(&pte_count_storage_[phys / PAGE_SIZE], __ATOMIC_RELAXED);
}

void PMM::refcount_inc(uint64_t phys) {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return;
    }
    __atomic_add_fetch(&refcount_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL);
}

bool PMM::refcount_dec_and_test(uint64_t phys) {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return false;  // unmanaged (device/IoPhys)
    }
    if (__atomic_sub_fetch(&refcount_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL) != 0) {
        return false;
    }
    int16_t live_pc = __atomic_load_n(&pte_count_storage_[phys / PAGE_SIZE], __ATOMIC_RELAXED);
    if (live_pc != 0) {
        cinux::lib::kpanic("[AUDIT] free phys=0x%lx pte_count=%d (still mapped)",
                           static_cast<unsigned long>(phys), static_cast<int>(live_pc));
    }
    if (cinux::mm::g_page_cache.contains_phys(phys)) {
        cinux::lib::kpanic("[AUDIT] free phys=0x%lx still in PageCache",
                           static_cast<unsigned long>(phys));
    }
    __atomic_store_n(&pte_count_storage_[phys / PAGE_SIZE], 0, __ATOMIC_RELAXED);
    auto g = lock_.guard();  // buddy_ not thread-safe; serialize vs alloc (96bd1ae did alloc side)
    buddy_.free(phys / PAGE_SIZE);
    return true;
}

bool PMM::refcount_dec_and_test_no_free(uint64_t phys) {
    // Same as refcount_dec_and_test but does NOT buddy_.free -- audit still
    // runs (bad free caught at dec time), only the free is deferred.  Caller
    // (drain kthread) frees via free_page() after the shootdown.
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return false;
    }
    if (__atomic_sub_fetch(&refcount_storage_[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL) != 0) {
        return false;
    }
    int16_t live_pc = __atomic_load_n(&pte_count_storage_[phys / PAGE_SIZE], __ATOMIC_RELAXED);
    if (live_pc != 0) {
        cinux::lib::kpanic("[AUDIT] free phys=0x%lx pte_count=%d (still mapped)",
                           static_cast<unsigned long>(phys), static_cast<int>(live_pc));
    }
    if (cinux::mm::g_page_cache.contains_phys(phys)) {
        cinux::lib::kpanic("[AUDIT] free phys=0x%lx still in PageCache",
                           static_cast<unsigned long>(phys));
    }
    // NOTE: do NOT store pte_count=0 / buddy_.free here -- caller frees after
    // shootdown.  pte_count is already 0 (audit above).
    return true;
}

int16_t PMM::refcount_load(uint64_t phys) const {
    if (phys == 0 || phys / PAGE_SIZE >= total_pages_) {
        return 0;
    }
    return __atomic_load_n(&refcount_storage_[phys / PAGE_SIZE], __ATOMIC_RELAXED);
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
