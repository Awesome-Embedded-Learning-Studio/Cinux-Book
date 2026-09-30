/**
 * @file    layout.hpp
 * @brief   Physical-mode layout facts shared by every boot stage.
 *
 * The constants here are contracts, not preferences: the BIOS always drops
 * the MBR at 0x7C00, stage2 always follows one sector behind, and the
 * debug console always answers on port 0xE9. Boot code and host-side
 * tooling include the same values so the image on disk and the code that
 * jumps into it never disagree. The static_assert pins the width those
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

#include "cinux/region/memory_region.hpp"

namespace cinux::boot {

static_assert(sizeof(unsigned short) == 2);

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

inline constexpr BootRegion kStage2Spot{.segments = 0x0000, .offset = 0x7E00, .sectors = 12};

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

inline constexpr unsigned long kKernelLoadLma = 0x20000;

inline constexpr unsigned long kPmStackTop = 0x90000;

static_assert(kPmStackTop % 4 == 0);
static_assert(kPmStackTop > kKernelLoadLma);
static_assert(kPmStackTop < 0x100000);
static_assert(static_cast<unsigned long>(kStage2Spot.offset) +
                  (static_cast<unsigned long>(kStage2Spot.sectors) * 512U) <
              0x100000);

static_assert(kPageTables.base >= kLowFree.base);
static_assert(kStage2Stack.top <= kLowFree.top);

// Semantic contracts: stage2 sits right behind the MBR, flat DS is design not luck
static_assert(kStage2Spot.offset == kMbrBase + 512);           //
static_assert(kStage2Spot.segments == 0x0000);                 // DS = 0 is flat mode
static_assert(kStage2Stack.segments == kStage2Spot.segments);  //
static_assert(kLowFree.top == kMbrBase);                       //
static_assert(kPageTables.base % 0x1000 == 0);
static_assert(kPageTables.top <= kStage2Stack.top);

}  // namespace cinux::boot
