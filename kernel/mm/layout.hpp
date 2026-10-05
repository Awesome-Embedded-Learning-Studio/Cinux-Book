/**
 * @file    layout.hpp
 * @brief   The published virtual address space map of the kernel.
 *
 * One header states where every region lives: the direct map of physical
 * RAM, the kernel heap, the vmalloc and ioremap reservations, the kernel
 * image window, and the user-half conventions. Every address the memory
 * subsystem hands out traces back to a constant here, and the
 * static_asserts below weld each base to its PML4 slot so the map and the
 * hardware arithmetic cannot drift apart.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"
#include "kernel/arch/x86_64/page.hpp"

namespace cinux::mm {

using cinux::base::operator""_GiB;
using cinux::base::operator""_KiB;

/** @brief Bytes one PML4 entry spans; every reserved region is sized in whole entries. */
inline constexpr unsigned long kPml4EntrySpan = 512_GiB;

/** @brief PML4 slot of the direct map of physical RAM. */
inline constexpr unsigned long kDirectMapPml4 = 256;
/** @brief PML4 slot of the kernel heap window. */
inline constexpr unsigned long kHeapPml4      = 257;
/** @brief PML4 slot reserved for vmalloc and kernel thread stacks. */
inline constexpr unsigned long kVmallocPml4   = 258;
/** @brief PML4 slot reserved for ioremap and device MMIO. */
inline constexpr unsigned long kIoremapPml4   = 259;
/** @brief PML4 slot holding the kernel image and module windows. */
inline constexpr unsigned long kKernelPml4    = 511;

/** @brief Base of the direct map: virtual = base + physical, fixed offset. */
inline constexpr unsigned long kDirectMapBase = 0xFFFF800000000000UL;
/** @brief Base of the kernel heap window; grows upward inside its entry. */
inline constexpr unsigned long kHeapBase      = 0xFFFF808000000000UL;
/** @brief Base of the vmalloc reservation; no allocator serves it yet. */
inline constexpr unsigned long kVmallocBase   = 0xFFFF810000000000UL;
/** @brief Base of the ioremap reservation; phase one mirrors physical addresses. */
inline constexpr unsigned long kIoremapBase   = 0xFFFF818000000000UL;

/** @brief PDPT slot of the kernel image window inside PML4 entry 511. */
inline constexpr unsigned long kKernelImagePdpt = 510;
/** @brief PDPT slot of the module reservation inside PML4 entry 511. */
inline constexpr unsigned long kModulePdpt      = 511;

/** @brief Base of the kernel image window; the image itself still links at +2 MiB. */
inline constexpr unsigned long kKernelImageBase = 0xFFFFFFFF80000000UL;
/** @brief Base of the module reservation; the -mcmodel=kernel link keeps code in the last 2 GiB. */
inline constexpr unsigned long kModuleBase      = 0xFFFFFFFFC0000000UL;

/** @brief Highest user-visible address plus one; PML4 entries 0..255 end here. */
inline constexpr unsigned long kUserTop          = 0x0000800000000000UL;
/** @brief Null-trap guard: user mappings never start below this address. */
inline constexpr unsigned long kUserNullTop      = 64_KiB;
/** @brief Default boundary between the program-plus-brk area and the mmap area. */
inline constexpr unsigned long kUserBrkMmapSplit = 0x0000400000000000UL;
/** @brief Default top of the mmap area; allocations grow down from here. */
inline constexpr unsigned long kUserMmapTop      = 0x00007F8000000000UL;
/** @brief Upper edge of the default main stack; the page above stays unmapped. */
inline constexpr unsigned long kUserStackTop     = kUserTop - cinux::arch::page::kSize;

/**
 * @brief         Turns a physical address into its direct-map alias.
 * @param[in]     physical   Address inside the mapped RAM span.
 * @return        Virtual address of the same byte in the direct map.
 * @note          Meaningful only for physical addresses the direct map covers.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
constexpr unsigned long DirectMapVirt(unsigned long physical) {
    return kDirectMapBase + physical;
}

/**
 * @brief         Recovers the physical address behind a direct-map pointer.
 * @param[in]     virtual_address   Address inside the direct-map entry.
 * @return        The physical byte the entry maps at that offset.
 * @note          Heap, vmalloc, and image addresses have no fixed offset;
 *                only the direct map converts by subtraction.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
constexpr unsigned long DirectMapPhys(unsigned long virtual_address) {
    return virtual_address - kDirectMapBase;
}

/**
 * @brief         Reports whether the address falls in the direct map's PML4 entry.
 * @param[in]     virtual_address   Address to classify.
 * @return        true inside [kDirectMapBase, kDirectMapBase + kPml4EntrySpan).
 * @note          Entry-wide membership; whether RAM is actually mapped there
 *                is the VMM's question, not the layout's.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
constexpr bool IsDirectMap(unsigned long virtual_address) {
    return virtual_address >= kDirectMapBase && virtual_address < kDirectMapBase + kPml4EntrySpan;
}

/**
 * @brief         Reports whether the address lives in the kernel half.
 * @param[in]     virtual_address   Address to classify.
 * @return        true from kDirectMapBase up to the top of the space.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
constexpr bool IsKernelHalf(unsigned long virtual_address) {
    return virtual_address >= kDirectMapBase;
}

/**
 * @brief         Turns a device physical address into its phase-one ioremap alias.
 * @param[in]     physical   MMIO address below the reservation's 512 GiB span.
 * @return        Virtual address inside the ioremap entry, same offset.
 * @note          Phase one mirrors physical offsets so early consumers such
 *                as the framebuffer need no allocator; a real ioremap that
 *                hands out tracked ranges replaces this when one is needed.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
constexpr unsigned long IoremapVirt(unsigned long physical) {
    return kIoremapBase + physical;
}

static_assert(kPml4EntrySpan == (1UL << 39), "one PML4 entry spans 2^39 bytes");
static_assert(kDirectMapBase == (0xFFFF000000000000UL | (kDirectMapPml4 << 39)),
              "direct map sits exactly at its PML4 slot");
static_assert(kHeapBase == kDirectMapBase + kPml4EntrySpan, "heap owns the next entry");
static_assert(kVmallocBase == kHeapBase + kPml4EntrySpan, "vmalloc owns the next entry");
static_assert(kIoremapBase == kVmallocBase + kPml4EntrySpan, "ioremap owns the next entry");
static_assert(kIoremapBase + kPml4EntrySpan <= 0xFFFFFF8000000000UL,
              "reserved entries stop before the kernel image entry");
static_assert(kKernelImageBase ==
                  (0xFFFF000000000000UL | (kKernelPml4 << 39) | (kKernelImagePdpt << 30)),
              "image window sits exactly at its PDPT slot");
static_assert(kModuleBase == (0xFFFF000000000000UL | (kKernelPml4 << 39) | (kModulePdpt << 30)),
              "module reservation sits exactly at its PDPT slot");
static_assert(kModuleBase == kKernelImageBase + 1_GiB, "modules follow the image window");
static_assert(kUserTop == (1UL << 47), "user half ends at the canonical boundary");
static_assert(kUserStackTop + cinux::arch::page::kSize == kUserTop,
              "one guard page separates the stack top from user top");
static_assert(kUserNullTop < kUserBrkMmapSplit && kUserBrkMmapSplit < kUserMmapTop &&
                  kUserMmapTop < kUserStackTop,
              "user conventions stay ordered inside the user half");

}  // namespace cinux::mm
