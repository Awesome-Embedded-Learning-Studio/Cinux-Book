/**
 * @file    gdt.hpp
 * @brief   The flat-model GDT contract for the protected-mode switch.
 *
 * The table is five entries: null, kernel code, kernel data, and the long
 * mode code/data pair. Base is zero and limit spans 4GB on both flat
 * descriptors, so once the far jump lands every address is its own linear
 * address. Byte layout is pinned by the static_asserts below and re-checked
 * in the LP64 host world by test_gdt,
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

/// Flags nibble of a long-mode code descriptor: G=1, D=0, L=1.
inline constexpr unsigned char kFlagsLongCode = 0xA;

/// Flags nibble of a long-mode data descriptor: G=1, D=0.
inline constexpr unsigned char kFlagsLongData = 0x8;

/// Selector of the fifth entry: the 64-bit code segment.
inline constexpr unsigned short kSelectorCode64 = 0x18;

/// Selector of the sixth entry: the 64-bit data segment.
inline constexpr unsigned short kSelectorData64 = 0x20;

struct [[gnu::packed]] BootGdt {
    SegmentDescriptor null;
    SegmentDescriptor code;
    SegmentDescriptor data;
    SegmentDescriptor code64;
    SegmentDescriptor data64;
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
 * @brief   Build the long-mode code descriptor: base 0, L=1, D=0.
 * @return  The encoded 8-byte descriptor (0x00AF9A000000FFFF).
 * @note    L=1 with D=0 is a hard architectural pairing for 64-bit code
 *          segments; the static_asserts below pin both bits.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr SegmentDescriptor MakeLongCodeDescriptor() {
    return SegmentDescriptor{
        .limit_low        = 0xFFFF,
        .base_low         = 0x0000,
        .base_mid         = 0x00,
        .access           = kAccessKernelCode,
        .flags_limit_high = static_cast<unsigned char>((kFlagsLongCode << 4) | 0x0F),
        .base_high        = 0x00};
}

/**
 * @brief   Build the long-mode data descriptor: base 0, D=0.
 * @return  The encoded 8-byte descriptor (0x008F92000000FFFF).
 * @note    Long mode ignores base and limit on data segments; the entry
 *          exists so the selector arithmetic stays uniform.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr SegmentDescriptor MakeLongDataDescriptor() {
    return SegmentDescriptor{
        .limit_low        = 0xFFFF,
        .base_low         = 0x0000,
        .base_mid         = 0x00,
        .access           = kAccessKernelData,
        .flags_limit_high = static_cast<unsigned char>((kFlagsLongData << 4) | 0x0F),
        .base_high        = 0x00};
}

/**
 * @brief   Build the five-entry boot GDT (null / code / data / long code /
 *          long data).
 * @return  The full 40-byte table value.
 * @note    The table is born with its long mode pair so the single lgdt of
 *          the protected-mode switch already admits the 64-bit selectors.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr BootGdt MakeBootGdt() {
    return BootGdt{.null   = SegmentDescriptor{},
                   .code   = MakeFlatDescriptor(kAccessKernelCode),
                   .data   = MakeFlatDescriptor(kAccessKernelData),
                   .code64 = MakeLongCodeDescriptor(),
                   .data64 = MakeLongDataDescriptor()};
}

static_assert(sizeof(SegmentDescriptor) == 8);
static_assert(sizeof(BootGdt) == 40);
static_assert(sizeof(DescriptorTablePointer) == 6);

constexpr BootGdt kTemplate = MakeBootGdt();

static_assert(kTemplate.code.base_low == 0 && kTemplate.code.base_mid == 0 &&
              kTemplate.code.base_high == 0);
static_assert(kTemplate.code.access == 0x9A && kTemplate.data.access == 0x92);
static_assert(kTemplate.code.flags_limit_high == 0xCF);

static_assert(kTemplate.code64.flags_limit_high == 0xAF);
static_assert(kTemplate.data64.flags_limit_high == 0x8F);
static_assert(((kTemplate.code64.flags_limit_high >> 5) & 1U) == 1U);
static_assert(((kTemplate.code64.flags_limit_high >> 6) & 1U) == 0U);

inline constexpr unsigned long kFlatLimitBytes = (0xFFFFFUL * 0x1000UL) + 0xFFFUL;

}  // namespace cinux::boot::gdt
