/**
 * @file kernel/drivers/dma/dma_pool.cpp
 * @brief DmaPool implementation
 */

#include "kernel/drivers/dma/dma_pool.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/pmm.hpp"

namespace cinux::drivers::dma {

namespace {

/// Whole-page count needed to hold @p bytes.
std::size_t pages_for(std::size_t bytes) {
    return (bytes + cinux::arch::PAGE_SIZE - 1) / cinux::arch::PAGE_SIZE;
}

}  // namespace

DmaPool g_dma_pool;

cinux::lib::ErrorOr<DmaBuffer> DmaPool::alloc(std::size_t size) {
    if (size == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    const std::size_t pages = pages_for(size);
    const uint64_t    phys  = cinux::mm::g_pmm.alloc_pages(pages);
    if (phys == 0) {
        return cinux::lib::Error::OutOfMemory;
    }

    // The direct map is a permanent identity mapping of all RAM, built once by
    // the loader's direct_map_up_to() (1 GB huge pages in PML4[272]).  virt =
    // phys + DIRECT_MAP_BASE is therefore already CPU-accessible and needs no
    // per-buffer PTE work.  We MUST NOT g_vmm.map() it here: VMM::map's
    // walk_level() would hit the 1 GB huge entry, split it into 4 KB pages, and
    // corrupt the shared direct map for the whole kernel -- the next
    // phys_to_virt() then takes a reserved-bit #PF (GOTCHA #7/#13).  Mirror of
    // return_pages(), which frees only the physical pages and never unmaps.
    const uint64_t virt = phys + cinux::arch::DIRECT_MAP_BASE;
    return DmaBuffer(phys, reinterpret_cast<void*>(virt), size, &DmaPool::release_callback);
}

void DmaPool::return_pages(const DmaBuffer& buf) {
    if (!buf.valid()) {
        return;
    }
    // Return the physical pages only.  The direct map (virt = phys +
    // DIRECT_MAP_BASE) is a permanent, loader-built identity mapping shared by
    // the whole kernel; we never allocate or free PTEs for it (see alloc()).
    // The freed phys may be reallocated later and reused as another DMA buffer,
    // which is fine: the same virt (phys + DIRECT_MAP_BASE) keeps working.
    cinux::mm::g_pmm.free_pages(buf.phys(), pages_for(buf.size()));
}

void DmaPool::free(DmaBuffer& buf) {
    if (!buf.valid()) {
        return;
    }
    return_pages(buf);
    // Surrender ownership so ~DmaBuffer won't release again.
    uint64_t    p;
    void*       v;
    std::size_t s;
    buf.detach(p, v, s);
}

void DmaPool::release_callback(const DmaBuffer& buf) {
    return_pages(buf);
    // ~DmaBuffer::release_owned clears the fields after this returns.
}

}  // namespace cinux::drivers::dma
