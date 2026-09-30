/**
 * @file    gdt.hpp
 * @brief   The flat-model GDT contract for the protected-mode switch.
 *
 * The table is three entries: null, kernel code, kernel data. Base is zero
 * and limit spans 4GB on both flat descriptors, so once the far jump lands
 * every address is its own linear address. Byte layout is pinned by the
 * static_asserts below and re-checked in the LP64 host world by test_gdt,
 * which is where packing drift gets caught before it reaches the boot image.
 *
 * @author  Charliechen114514
 * @date    2026-09-30
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_gdt
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::boot::gdt {

struct [[gnu::packed]] SegmentDescriptor {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  flags_limit_high;
    unsigned char  base_high;
};

struct [[gnu::packed]] DescriptorTablePointer {
    unsigned short limit;
    unsigned int   base;
};

inline constexpr unsigned char  kAccessKernelCode = 0x9A;
inline constexpr unsigned char  kAccessKernelData = 0x92;
inline constexpr unsigned char  kFlagsFlat32      = 0xC;
inline constexpr unsigned short kSelectorCode     = 0x08;
inline constexpr unsigned short kSelectorData     = 0x10;

struct [[gnu::packed]] BootGdt {
    SegmentDescriptor null;
    SegmentDescriptor code;
    SegmentDescriptor data;
};

/**
 * @brief   Build one flat descriptor: base 0, limit 4GB, given access byte.
 * @param   [in] access  Access byte: 0x9A for kernel code, 0x92 for data.
 * @return  The encoded 8-byte descriptor.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr SegmentDescriptor MakeFlatDescriptor(unsigned char access) {
    return SegmentDescriptor{
        .limit_low        = 0xFFFF,
        .base_low         = 0x0000,
        .base_mid         = 0x00,
        .access           = access,
        .flags_limit_high = static_cast<unsigned char>((kFlagsFlat32 << 4) | 0x0F),
        .base_high        = 0x00};
}

/**
 * @brief   Build the three-entry boot GDT (null / kernel code / kernel data).
 * @return  The full 24-byte table value.
 * @note    Station 05 rebuilds the table for long mode; nothing here is
 *          forward-compatible by design, the descriptor count is exactly
 *          what the switch needs.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr BootGdt MakeBootGdt() {
    return BootGdt{.null = SegmentDescriptor{},
                   .code = MakeFlatDescriptor(kAccessKernelCode),
                   .data = MakeFlatDescriptor(kAccessKernelData)};
}

static_assert(sizeof(SegmentDescriptor) == 8);
static_assert(sizeof(BootGdt) == 24);
static_assert(sizeof(DescriptorTablePointer) == 6);

constexpr BootGdt kTemplate = MakeBootGdt();

static_assert(kTemplate.code.base_low == 0 && kTemplate.code.base_mid == 0 &&
              kTemplate.code.base_high == 0);
static_assert(kTemplate.code.access == 0x9A && kTemplate.data.access == 0x92);
static_assert(kTemplate.code.flags_limit_high == 0xCF);

inline constexpr unsigned long kFlatLimitBytes = (0xFFFFFUL * 0x1000UL) + 0xFFFUL;

}  // namespace cinux::boot::gdt
