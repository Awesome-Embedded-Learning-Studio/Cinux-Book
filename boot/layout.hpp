/**
 * @file    layout.hpp
 * @brief   Physical-mode layout facts shared by every boot stage.
 *
 * The constants here are contracts, not preferences: the BIOS always drops
 * the MBR at 0x7C00, stage2 always follows one sector behind, and the
 * debug console always answers on port 0xE9. Boot code and host-side
 * tooling include the same values so the image on disk and the code that
 * jumps into it never disagree. test_layout pins the width those
 * segment:offset values rely on — real-mode arithmetic assumes a 16-bit
 * unsigned short, and a toolchain that widens it silently changes the
 * layout every consumer agreed on.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_layout
 */

#pragma once

#include "cinux/ptr.hpp"
#include "cinux/region/memory_region.hpp"

namespace cinux::boot {

/**
 * @brief   Where a boot image lands in real-mode memory and how big it is.
 * @note    None
 * @since   0.1.0
 * @ingroup boot_layout
 */
struct BootRegion {
    unsigned short segments;
    unsigned short offset;
    unsigned short sectors;
};

inline constexpr BootRegion kStage2Spot{.segments = 0x0000, .offset = 0x7E00, .sectors = 28};

/// Linear address the BIOS loads the MBR's first sector to.
inline constexpr unsigned short kMbrBase = 0x7C00;

using base::memory::MemoryRegion;

inline constexpr MemoryRegion kLowFree{0x0500, 0x7C00};

inline constexpr MemoryRegion kPageTables{0x1000, 0x4000};

/**
 * @brief   Where a boot-time stack lives: segment plus downward-growing top.
 * @note    None
 * @since   0.1.0
 * @ingroup boot_layout
 */
struct BootStack {
    unsigned short segments;
    unsigned short top;
};
inline constexpr BootStack kStage2Stack{.segments = 0x0000, .top = 0x7000};

inline constexpr unsigned long kPmStackTop = 0x90000;

/// Where the separately linked 64-bit blob lands inside the stage2 image;
/// the far-jump target of the long-mode switch and the blob's own link base.
inline constexpr unsigned long kLmEntryVma = 0xB000;

inline constexpr unsigned long kFerryWindow     = 0x10000;
inline constexpr unsigned long kFerryWindowSize = 0x10000;
inline constexpr unsigned long kKernelImageLba  = 32;
inline constexpr unsigned long kHeaderScratch   = 0x4000;

inline constexpr unsigned long      kHandoffMailboxEntry = 0x4F00;
inline constexpr unsigned long      kHandoffMailboxInfo  = 0x4F08;
inline constexpr unsigned long long kHighHalfBase        = 0xFFFFFFFF80000000ULL;

/**
 * @brief         Reads one word from a fixed physical address.
 *
 * @param[in]     address   Linear address to read.
 * @return        The word stored there.
 * @note          volatile by contract: boot mailboxes are written by one
 *                world and read by another, so the read must happen.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_layout
 */
inline unsigned long LoadWord(unsigned long address) {
    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
    return *base::PtrAt<const volatile unsigned long>(address);
}

/**
 * @brief         Writes one word to a fixed physical address.
 *
 * @param[in]     address   Linear address to write.
 * @param[in]     value     Word to store.
 * @return        None
 * @note          volatile by contract: the write must land before any
 *                world switch that consumes it.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       boot_layout
 */
inline void StoreWord(unsigned long address, unsigned long value) {
    // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
    *base::PtrAt<volatile unsigned long>(address) = value;
}

// Semantic contracts: stage2 sits right behind the MBR, flat DS is design not luck

}  // namespace cinux::boot
