/**
 * @file    page_entry.hpp
 * @brief   The x86-64 page-table entry grammar shared by boot and the VMM.
 *
 * An entry is a BitMask<uint64_t> pinned to eight bytes: the width is a
 * hardware fact, not a property of the compiling world, and the pin holds
 * in every world that includes this header. Constructors take a physical
 * address and store the frame number, so address-to-field translation
 * happens once, here, next to the layout constants it derives from.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_page
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::base::page {

/// One page-table entry; eight bytes in every compiling world.
using Entry = bit::BitMask<uint64_t>;

/// Bit 0: the entry maps something.
inline constexpr Entry kPresent = bit::MaskBit<uint64_t>(0);

/// Bit 1: the mapping is writable.
inline constexpr Entry kWritable = bit::MaskBit<uint64_t>(1);

/// Bit 7: this entry is a large page, not a pointer to the next table.
inline constexpr Entry kLarge = bit::MaskBit<uint64_t>(7);

/// Shift of a 2MB large page: frame numbers below count in 2MB units.
inline constexpr unsigned char kLargePageShift = 21;

/// Size of one 2MB large page.
inline constexpr uint64_t kLargePageSize = 1ULL << kLargePageShift;

/// Physical address field of a table-pointer entry: bits 51..12.
inline constexpr bit::BitRange kTablePhys{.low = 12, .width = 40};

/// Physical address field of a large-page entry: bits 51..21.
inline constexpr bit::BitRange kLargePagePhys{.low = 21, .width = 31};

/**
 * @brief         Build a table-pointer entry: present plus the given flags,
 *                physical field holding the 4KB frame number of the next
 *                table.
 *
 * @param[in]     physical   Physical address of the next table; low bits
 *                           below 12 are dropped.
 * @param[in]     flags      Extra entry flags, typically kWritable.
 * @return        The encoded entry.
 * @since         0.1.0
 * @ingroup       base_page
 */
constexpr Entry MakeTableEntry(uint64_t physical, Entry flags) {
    Entry entry{kPresent | flags};
    entry.deposit(kTablePhys, physical >> kTablePhys.low);
    return entry;
}

/**
 * @brief         Build a 2MB large-page entry: present plus large plus the
 *                given flags, physical field holding the 2MB frame number.
 *
 * @param[in]     physical   Physical address of the page; low bits below 21
 *                           are dropped.
 * @param[in]     flags      Extra entry flags, typically kWritable.
 * @return        The encoded entry.
 * @since         0.1.0
 * @ingroup       base_page
 */
constexpr Entry MakeLargePageEntry(uint64_t physical, Entry flags) {
    Entry entry{kPresent | kLarge | flags};
    entry.deposit(kLargePagePhys, physical >> kLargePagePhys.low);
    return entry;
}

static_assert(sizeof(Entry) == 8);
static_assert(kLargePageSize == 0x200000);
static_assert(MakeTableEntry(0x2000, kWritable).raw == 0x2003);
static_assert(MakeTableEntry(0x2007, kWritable).raw == 0x2003);
static_assert(MakeLargePageEntry(0x0, kWritable).raw == 0x83);
static_assert(MakeLargePageEntry(kLargePageSize, kWritable).raw == 0x200083);
static_assert(MakeLargePageEntry(0x200123, kWritable).raw == 0x200083);

}  // namespace cinux::base::page
