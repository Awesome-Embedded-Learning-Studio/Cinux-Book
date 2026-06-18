/**
 * @file kernel/arch/x86_64/phys_virt.hpp
 * @brief Physical-to-virtual translation via the direct map
 *
 * Single source of truth for phys_to_virt().  The direct map (DIRECT_MAP_BASE,
 * identity-mapped for all RAM by the mini loader) covers every physical page the
 * PMM can return, unlike the KERNEL_VMA higher-half window which the bootloader
 * caps at 1 GB.  Use this for arbitrary phys->virt (page tables, DMA buffers,
 * page contents).  Kernel-image-relative arithmetic still uses KERNEL_VMA -- the
 * kernel image is linked and mapped at KERNEL_VMA + KERNEL_LMA, independent of
 * the direct map.
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"

/// Translate a physical address to its kernel-virtual address through the direct
/// map (DIRECT_MAP_BASE + phys).  Returns PageEntry* for page-table access; cast
/// for other pointer types.
inline cinux::arch::PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<cinux::arch::PageEntry*>(phys + cinux::arch::DIRECT_MAP_BASE);
}
