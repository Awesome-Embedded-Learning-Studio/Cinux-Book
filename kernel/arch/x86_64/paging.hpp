/**
 * @file kernel/arch/x86_64/paging.hpp
 * @brief Minimal paging helpers for the big kernel
 *
 * Provides functions to map physical memory regions into the virtual
 * address space by manipulating the page tables established by the
 * bootloader.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

/**
 * @brief Map a physical address range into the virtual address space
 *
 * Extends the page table with 2MB huge pages (first 1GB) or
 * 1GB huge pages (beyond 1GB) so that [phys, phys+size) is
 * accessible via both identity-mapped and higher-half virtual addresses.
 *
 * @param phys  Physical base address to map
 * @param size  Size of the region in bytes
 */
void map_mmio(uint64_t phys, uint64_t size);

}  // namespace cinux::arch
