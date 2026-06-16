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
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::dma {

namespace {

// DMA buffers are kernel-writable and present.
constexpr uint64_t kDmaFlags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;

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

    // Map every page into the higher-half direct-map window.  virt = phys +
    // KERNEL_VMA is unique per phys, so no virtual allocator is needed.
    const uint64_t virt = phys + cinux::arch::KERNEL_VMA;
    for (std::size_t i = 0; i < pages; i++) {
        const uint64_t v = virt + i * cinux::arch::PAGE_SIZE;
        const uint64_t p = phys + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(v, p, kDmaFlags)) {
            // Rollback: unmap what we mapped, return the physical allocation.
            for (std::size_t j = 0; j < i; j++) {
                cinux::mm::g_vmm.unmap(virt + j * cinux::arch::PAGE_SIZE);
            }
            cinux::mm::g_pmm.free_pages(phys, pages);
            return cinux::lib::Error::OutOfMemory;
        }
    }

    return DmaBuffer(phys, reinterpret_cast<void*>(virt), size, &DmaPool::release_callback);
}

void DmaPool::return_pages(const DmaBuffer& buf) {
    if (!buf.valid()) {
        return;
    }
    // Return the physical pages only.  The higher-half direct map
    // (virt = phys + KERNEL_VMA) is a permanent phys<->virt correspondence
    // shared by the whole kernel -- unmapping a DMA buffer's window would
    // corrupt it and send later demand paging into an infinite remap loop (the
    // freed slot faults, the handler maps a *different* phys, faults again...).
    // The freed phys may be reallocated later; if it returns as a DMA buffer,
    // alloc()'s map() re-establishes the same PTE (idempotent overwrite).  This
    // mirrors how the AHCI driver holds its command-list/FIS mappings.
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
