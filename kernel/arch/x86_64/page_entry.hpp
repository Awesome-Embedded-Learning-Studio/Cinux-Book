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
#include "cinux/literal_types.hpp"

namespace cinux::arch::page {

using cinux::base::operator""_GiB;
using cinux::base::operator""_MiB;

/// One page-table entry; eight bytes in every compiling world.
using Entry = base::bit::BitMask<uint64_t>;

/// Bit 0: the entry maps something.
inline constexpr Entry kPresent = base::bit::MaskBit<uint64_t>(0);

/// Bit 1: the mapping is writable.
inline constexpr Entry kWritable = base::bit::MaskBit<uint64_t>(1);

/// Bit 7: this entry is a large page, not a pointer to the next table.
inline constexpr Entry kLarge = base::bit::MaskBit<uint64_t>(7);

/// Shift of a 2MB large page: frame numbers below count in 2MB units.
inline constexpr unsigned char kLargePageShift = 21;

/// Size of one 2MB large page.
inline constexpr uint64_t kLargePageSize = 2_MiB;

/// Shift of a 1GB huge page: door indexes below count in 1GB units.
inline constexpr unsigned char kHugePageShift = 30;

/// Size of one 1GB huge page.
inline constexpr uint64_t kHugePageSize = 1_GiB;

/// Physical address field of a table-pointer entry: bits 51..12.
inline constexpr base::bit::BitRange kTablePhys{.low = 12, .width = 40};

/// Physical address field of a large-page entry: bits 51..21.
inline constexpr base::bit::BitRange kLargePagePhys{.low = 21, .width = 31};

/// Physical address field of a huge-page entry: bits 51..30.
inline constexpr base::bit::BitRange kHugePagePhys{.low = 30, .width = 22};

static_assert(kLargePageSize == (1ULL << kLargePageShift));
static_assert(kHugePageSize == (1ULL << kHugePageShift));
static_assert(kLargePageShift == kLargePagePhys.low);
static_assert(kHugePageShift == kHugePagePhys.low);

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

/**
 * @brief         Build a 1GB huge-page entry: present plus large plus the
 *                given flags, physical field holding the 1GB frame number.
 *
 * @param[in]     physical   Physical address of the page; must be 1GB
 *                           aligned, low bits below 30 are dropped.
 * @param[in]     flags      Extra entry flags, typically kWritable.
 * @return        The encoded entry.
 * @since         0.1.0
 * @ingroup       base_page
 */
constexpr Entry MakeHugePageEntry(uint64_t physical, Entry flags) {
    Entry entry{kPresent | kLarge | flags};
    entry.deposit(kHugePagePhys, physical >> kHugePagePhys.low);
    return entry;
}

/// Which huge-page doors a device region needs in one page-directory
/// pointer table.
struct DoorSpan {
    /// First page-directory-pointer index the region touches.
    uint64_t first_index;

    /// How many 1GB huge-page entries cover the region.
    uint64_t count;
};

/**
 * @brief         Plan the 1GB huge-page doors for one device region, such
 *                as the VBE linear framebuffer: the first
 *                page-directory-pointer index and how many entries, ends
 *                rounded outward so a region straddling a 1GB edge stays
 *                fully mapped.
 *
 * @param[in]     physical   Physical start of the region.
 * @param[in]     bytes      Size of the region in bytes; zero yields zero
 *                           doors.
 * @return        The door span inside one page-directory pointer table.
 * @note          Assumes physical + bytes does not wrap the address space
 *                and the first index stays below 512, the width of one
 *                page-directory pointer table; real device regions sit
 *                far below both.
 * @since         0.1.0
 * @ingroup       base_page
 */
constexpr DoorSpan PlanDeviceDoors(uint64_t physical, uint64_t bytes) {
    DoorSpan span{.first_index = physical >> kHugePageShift, .count = 0};
    if (bytes == 0) {
        return span;
    }
    uint64_t const kLast = (physical + bytes - 1) >> kHugePageShift;
    span.count           = kLast - span.first_index + 1;
    return span;
}

/**
 * @brief         The 1GB window base a physical byte lives in.
 *
 * @param[in]     physical   Any physical address.
 * @return        Base of the huge-page window containing it.
 * @note          The named unit for walking RAM or MMIO one 1GB window
 *                at a time, as the bring-up and door planners do.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_page
 */
constexpr uint64_t HugeWindowBase(uint64_t physical) {
    return physical / kHugePageSize * kHugePageSize;
}

/**
 * @brief         The 2MB large-page base a physical byte lives in.
 *
 * @param[in]     physical   Any physical address.
 * @return        Base of the large page containing it.
 * @note          The named unit for walking RAM one 2MB page at a time
 *                when filling a page directory.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_page
 */
constexpr uint64_t LargePageBase(uint64_t physical) {
    return physical / kLargePageSize * kLargePageSize;
}

}  // namespace cinux::arch::page
