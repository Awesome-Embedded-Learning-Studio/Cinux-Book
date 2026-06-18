/**
 * @file kernel/mm/buddy.cpp
 * @brief Buddy-system physical page allocator implementation (F2-M7)
 *
 * Namespace: cinux::mm
 */

#include "kernel/mm/buddy.hpp"

namespace cinux::mm {

// ============================================================
// Initialisation
// ============================================================

uint64_t BuddyAllocator::bitmap_bytes(uint64_t total_pages) {
    uint64_t bytes = 0;
    for (int o = 0; o <= kMaxOrder; o++) {
        uint64_t bits = total_pages >> o;
        bytes += (((bits + 7) / 8) + 7) & ~static_cast<uint64_t>(7);  // 8-byte aligned
    }
    return bytes;
}

void BuddyAllocator::init(uint64_t base_phys, uint64_t total_pages, uint8_t* order_storage,
                          uint8_t* bitmap_storage) {
    base_phys_   = base_phys;
    total_pages_ = total_pages;
    order_       = order_storage;
    free_pages_  = 0;

    // Lay out one zeroed bitmap per order (8-byte aligned) inside bitmap_storage.
    uint8_t* p = bitmap_storage;
    for (int o = 0; o <= kMaxOrder; o++) {
        free_bitmap_[o] = reinterpret_cast<uint64_t*>(p);
        uint64_t bits  = blocks_per_order(o);
        uint64_t bytes = ((bits + 7) / 8 + 7) & ~static_cast<uint64_t>(7);
        uint64_t words = bytes / sizeof(uint64_t);
        for (uint64_t w = 0; w < words; w++)
            free_bitmap_[o][w] = 0;
        p += bytes;
    }
    for (uint64_t i = 0; i < total_pages; i++)
        order_[i] = kNotAllocatedHead;  // nothing allocated yet
}

int BuddyAllocator::largest_fitting_order(uint64_t page, uint64_t remaining) {
    int o = 0;
    while (o < kMaxOrder) {
        uint64_t next_size = 1ULL << (o + 1);
        if ((page & (next_size - 1)) == 0 && remaining >= next_size)
            o++;
        else
            break;
    }
    return o;
}

void BuddyAllocator::mark_free_region(uint64_t base_page, uint64_t count) {
    uint64_t p   = base_page;
    uint64_t end = base_page + count;
    while (p < end) {
        int o = largest_fitting_order(p, end - p);
        set_bit(o, p >> o);
        free_pages_ += (1ULL << o);
        p += (1ULL << o);
    }
}

// ============================================================
// Bitmap primitives
// ============================================================

void BuddyAllocator::set_bit(int o, uint64_t block) {
    free_bitmap_[o][block / 64] |= (1ULL << (block % 64));
}

void BuddyAllocator::clear_bit(int o, uint64_t block) {
    free_bitmap_[o][block / 64] &= ~(1ULL << (block % 64));
}

bool BuddyAllocator::test_bit(int o, uint64_t block) const {
    return (free_bitmap_[o][block / 64] >> (block % 64)) & 1ULL;
}

uint64_t BuddyAllocator::find_first_set(int o) const {
    uint64_t words = (blocks_per_order(o) + 63) / 64;
    for (uint64_t w = 0; w < words; w++) {
        uint64_t v = free_bitmap_[o][w];
        if (v != 0) {
            uint64_t bit   = __builtin_ctzll(v);
            uint64_t block = w * 64 + bit;
            if (block < blocks_per_order(o))
                return block;
        }
    }
    return kInvalidPage;
}

void BuddyAllocator::mark_head_allocated(uint64_t page, int order) {
    uint64_t size = 1ULL << order;
    order_[page]  = static_cast<uint8_t>(order);
    for (uint64_t i = 1; i < size && (page + i) < total_pages_; i++) {
        order_[page + i] = kNotAllocatedHead;  // mark interior, so stray frees are no-ops
    }
}

// ============================================================
// Allocation
// ============================================================

uint64_t BuddyAllocator::alloc_order(int order) {
    if (order < 0 || order > kMaxOrder)
        return kInvalidPage;

    // Find the smallest order at or above the request that has a free block.
    int o = order;
    while (o <= kMaxOrder && find_first_set(o) == kInvalidPage)
        o++;
    if (o > kMaxOrder)
        return kInvalidPage;  // OOM

    uint64_t block = find_first_set(o);
    clear_bit(o, block);
    uint64_t page = block << o;

    // Split down to the requested order: the upper half of each level goes free.
    while (o > order) {
        o--;
        set_bit(o, (page + (1ULL << o)) >> o);  // upper half block at this lower order
    }

    mark_head_allocated(page, order);
    free_pages_ -= (1ULL << order);
    return page;
}

// ============================================================
// Free + coalescing
// ============================================================

void BuddyAllocator::free(uint64_t page) {
    if (page >= total_pages_)
        return;
    uint8_t recorded = order_[page];
    if (recorded == kNotAllocatedHead)
        return;  // not an allocated head: double-free / interior / reserved
    int order = static_cast<int>(recorded);

    free_pages_ += (1ULL << order);
    uint64_t size = 1ULL << order;
    for (uint64_t i = 0; i < size && (page + i) < total_pages_; i++)
        order_[page + i] = kNotAllocatedHead;

    // Coalesce with buddies as far as the alignment invariant allows.
    while (order < kMaxOrder) {
        uint64_t b = buddy_of(page, order);
        if (b >= total_pages_)
            break;  // buddy outside the managed range (region edge)
        if (!test_bit(order, b >> order))
            break;  // buddy not a free block of this order -> cannot merge
        clear_bit(order, b >> order);
        page = (b < page) ? b : page;  // merged block head is the lower address
        order++;
    }

    set_bit(order, page >> order);
}

}  // namespace cinux::mm
