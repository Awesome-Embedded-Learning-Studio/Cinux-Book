/**
 * @file    heap.hpp
 * @brief   The byte-granular allocator that lives on mapped pages.
 *
 * One span of virtual memory, cut into blocks: each block carries a
 * header with its payload size and state, a footer repeating the span
 * for backward walks, and free blocks additionally thread an
 * address-sorted free list. Allocate searches that list first-fit and
 * splits oversized blocks; Free validates the block, returns it, and
 * coalesces with whichever neighbor is free; Grow appends fresh space
 * at the top for the kernel side to hand in after mapping new pages.
 * The class knows arithmetic, not page tables — bringing pages in is
 * the runtime's job, so the same code runs in the host test world.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/singleton.hpp"


namespace cinux::mm {

/**
 * @brief   The span allocator behind the kernel heap.
 * @note    One instance owns one contiguous span; the kernel-wide one is
 *          Heap::self(), host tests construct their own over a pool.
 * @since   0.1.0
 * @ingroup kernel_mm
 */
class Heap : public cinux::base::Singleton<Heap> {
    friend class cinux::base::Singleton<Heap>;

public:
    /**
     * @brief         Claims a span as the heap's whole world.
     *
     * @param[in]     base    Span start; the caller guarantees the memory
     *                        is already mapped and writable.
     * @param[in]     bytes   Span length, at least one minimal block.
     * @return        true when the span became one free block.
     * @note          Re-initializing forgets every earlier allocation.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] bool init(unsigned long base, unsigned long bytes);

    /**
     * @brief         Hands out a block of at least the asked bytes.
     *
     * @param[in]     bytes   Payload wanted; rounded up internally.
     * @return        Sixteen-aligned payload pointer, or nullptr when
     *                no free block fits (the caller may grow and retry).
     * @note          First-fit over the address-sorted free list;
     *                oversized blocks split when the tail could host
     *                another minimal block.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    [[nodiscard]] void* allocate(unsigned long bytes);

    /**
     * @brief         Takes a block back into the free population.
     *
     * @param[in]     block   Payload pointer earlier obtained here.
     * @return        true when accepted; false when the pointer is not
     *                a live block of this heap (double free included).
     * @note          Accepted blocks coalesce with any free neighbor
     *                on both sides.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    bool free(void* block);

    /**
     * @brief         Extends the span's top after new pages arrived.
     *
     * @param[in]     new_top   New exclusive end of the span; must be
     *                          above and reach the old top exactly.
     * @return        true when the extension became free space.
     * @note          The extension coalesces with the current tail
     *                block when it is free.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       kernel_mm
     */
    bool grow(unsigned long new_top);

    /** @brief   Payload bytes currently free across all free blocks. */
    [[nodiscard]] unsigned long free_bytes() const;

    /** @brief   The span's exclusive top address. */
    [[nodiscard]] unsigned long top() const;

private:
    unsigned long base_       = 0;
    unsigned long top_        = 0;
    unsigned long free_head_  = 0;
    unsigned long free_bytes_ = 0;
};

}  // namespace cinux::mm
