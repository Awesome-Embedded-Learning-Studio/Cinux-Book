/**
 * @file kernel/mm/buddy.hpp
 * @brief Buddy-system physical page allocator (F2-M7)
 *
 * Replaces the PMM's flat bitmap with power-of-two order free lists.  Free
 * blocks are tracked by a per-order bitmap (1 bit per block) stored in caller-
 * provided metadata memory -- NOT inside the free pages themselves.  An earlier
 * "intrusive" design stored the free-list link pointer in the free page's own
 * direct-mapped header, but WSL2 nested-KVM (AMD) EPT does not always make a
 * sub-page write inside a huge page read back consistently, corrupting the
 * link (F2-M7 Bug2).  Keeping the bitmap in ordinary allocated metadata memory
 * avoids that entirely (GOTCHA #13/#14).  As a bonus, find_first_set() hands
 * out the lowest page index naturally -- no separate low-first logic needed.
 *
 * Each block's allocation order is recorded in a separate byte array (one byte
 * per page) at the block's head page, making free() authoritative: callers need
 * not remember the order -- coalescing is driven by the recorded order.  A
 * sentinel marks every page that is not an allocated head.
 *
 * The allocator is NOT thread-safe; the owning PMM serialises access (spinlock
 * for normal callers, exclusion-via-IF=0 for the page-fault path).
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"

namespace cinux::mm {

class BuddyAllocator {
public:
    /// Largest block is 2^kMaxOrder pages (8 MiB at 4 KiB pages).
    static constexpr int kMaxOrder = 11;

    /// Sentinel stored in the order array for any page that is not the head of
    /// an allocated block (free pages, interiors, reserved RAM).
    static constexpr uint8_t kNotAllocatedHead = 0xFF;

    /// Returned by alloc_order() when no block of the requested order fits.
    static constexpr uint64_t kInvalidPage = UINT64_MAX;

    /// Set up an empty allocator over page indices [0, total_pages) of
    /// @p base_phys.  @p order_storage (1 byte/page) records each head's order;
    /// @p bitmap_storage holds the per-order free bitmaps (sized via
    /// bitmap_bytes(total_pages)).  Both must outlive the allocator.
    void init(uint64_t base_phys, uint64_t total_pages, uint8_t* order_storage,
              uint8_t* bitmap_storage);

    /// Mark page indices [base_page, base_page + count) as free, splitting the
    /// span into the largest buddy blocks that keep the power-of-two alignment
    /// invariant.  Pages never marked free stay invisible to the allocator.
    void mark_free_region(uint64_t base_page, uint64_t count);

    /// Allocate a block of 2^order pages.  Returns the head page index, or
    /// kInvalidPage on OOM / bad order.  Caller holds exclusion.
    uint64_t alloc_order(int order);

    /// Free the block whose head is @p page.  The order is taken from recorded
    /// metadata (authoritative), and the block is coalesced with its buddies as
    /// far as the alignment invariant allows.  No-op if @p page is not an
    /// allocated head (double-free, interior page, reserved page, out of range).
    void free(uint64_t page);

    uint64_t free_pages() const { return free_pages_; }
    uint64_t total_pages() const { return total_pages_; }

    /// Number of bytes needed for the per-order free bitmaps over @p total_pages.
    static uint64_t bitmap_bytes(uint64_t total_pages);

private:
    uint8_t*  order_{nullptr};  ///< [page] -> order (allocated head) or kNotAllocatedHead
    uint64_t* free_bitmap_[kMaxOrder + 1]{};  ///< [order] -> 1 bit per block (1 = free)
    uint64_t  base_phys_{0};
    uint64_t  total_pages_{0};
    uint64_t  free_pages_{0};

    uint64_t   blocks_per_order(int o) const { return total_pages_ >> o; }
    void       set_bit(int o, uint64_t block);
    void       clear_bit(int o, uint64_t block);
    bool       test_bit(int o, uint64_t block) const;
    uint64_t   find_first_set(int o) const;  ///< lowest free block, or kInvalidPage
    void       mark_head_allocated(uint64_t page, int order);
    static int largest_fitting_order(uint64_t page, uint64_t remaining);
    static uint64_t buddy_of(uint64_t page, int order) { return page ^ (1ULL << order); }
};

}  // namespace cinux::mm
