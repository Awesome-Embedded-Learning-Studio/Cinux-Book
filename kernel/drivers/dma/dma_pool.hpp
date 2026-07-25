/**
 * @file kernel/drivers/dma/dma_pool.hpp
 * @brief DmaPool -- allocator for DMA-capable memory backed by PMM + VMM
 *
 * DmaPool hands out DmaBuffer handles whose backing memory is contiguous
 * physical pages from the PMM, mapped into the direct-map window
 * (virt = phys + DIRECT_MAP_BASE) via VMM::map.  Because the bus address uniquely
 * determines the virtual address, the pool needs no virtual address allocator
 * and cannot leak virtual space.  VMM::map overwrites any prior PTE for the
 * address, so the explicit map is safe whether or not the boot-time direct map
 * already covers the page.
 *
 * Each returned DmaBuffer carries a release hook: destruction (or reassignment)
 * returns its physical pages to the PMM automatically.  The direct-map PTEs are
 * intentionally left in place (the higher-half direct map is a permanent
 * phys<->virt window shared by the whole kernel; unmapping a slot corrupts it).
 * The pool's free() is only for explicit early release.
 *
 * Alignment: the PMM hands out page-aligned (4 KiB) memory, which satisfies
 * every common device alignment (AHCI cmd table 128 B, FIS 256 B, PRDT 4 KiB).
 * Sub-page slab allocation is out of scope; a request is rounded up to whole
 * pages, so asking for 256 B consumes a full page.
 *
 * Lifecycle: a DmaPool produces buffers only after the PMM and VMM are
 * initialised.  Early-boot ad-hoc DMA (mini loader, main.cpp sector buffer)
 * cannot use the pool and must stay manual.
 *
 * Namespace: cinux::drivers::dma
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstddef>

#include "kernel/drivers/dma/dma_buffer.hpp"

namespace cinux::drivers::dma {

/**
 * @brief Allocator for DMA-capable memory (whole-page, RAII-managed)
 *
 * @see DmaBuffer for the handle type returned by alloc().
 */
class DmaPool {
public:
    /**
     * @brief Allocate @p size bytes of DMA memory (rounded up to whole pages)
     * @return A DmaBuffer owning the mapping, or Error::OutOfMemory /
     *         Error::InvalidArgument (size == 0).  The buffer self-releases on
     *         destruction.
     */
    cinux::lib::ErrorOr<DmaBuffer> alloc(std::size_t size);

    /**
     * @brief Explicitly release a buffer's pages now
     *
     * Optional -- the DmaBuffer destructor already releases automatically via
     * the release hook.  Leaves @p buf invalid.
     */
    void free(DmaBuffer& buf);

private:
    /// Unmap and free the pages backing @p buf (no field mutation).
    static void return_pages(const DmaBuffer& buf);

    /// Release hook stored in each DmaBuffer; returns its pages on destruction.
    static void release_callback(const DmaBuffer& buf);
};

/// Global DMA pool instance.  Usable after PMM + VMM init.
extern DmaPool g_dma_pool;

}  // namespace cinux::drivers::dma
