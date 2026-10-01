/**
 * @file    gdt.hpp
 * @brief   The flat-model GDT contract for the protected-mode switch.
 *
 * The table is six entries: null, kernel code, kernel data, the long
 * mode code/data pair, and the 16-bit code segment the ferry returns
 * through. Base is zero and limit spans 4GB on both flat
 * descriptors, so once the far jump lands every address is its own linear
 * address. Byte layout is re-checked
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

/// Selector of the fourth entry: the 64-bit code segment.
inline constexpr unsigned short kSelectorCode64 = 0x18;

/// Selector of the fifth entry: the 64-bit data segment.
inline constexpr unsigned short kSelectorData64 = 0x20;

/// Flags nibble of a 16-bit PM code descriptor: G=1, D=0, L=0.
inline constexpr unsigned char kFlagsCode16 = 0x8;

/// Selector of the sixth entry: the 16-bit PM code segment (ferry return).
inline constexpr unsigned short kSelectorCode16 = 0x28;

struct [[gnu::packed]] BootGdt {
    SegmentDescriptor null;
    SegmentDescriptor code;
    SegmentDescriptor data;
    SegmentDescriptor code64;
    SegmentDescriptor data64;
    SegmentDescriptor code16;
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
 *          segments; test_gdt pins both bits.
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
 * @brief   Build the 16-bit code descriptor for the ferry's way back to
 *          real mode.
 * @return  The selector-0x28 descriptor value.
 * @note    D=0 with a byte-granular 64KB limit: executing under it after
 *          the far jump resets the CPU's idea of operand size, which is
 *          what lets the PM switch return to BIOS services.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr SegmentDescriptor MakeCode16Descriptor() {
    return SegmentDescriptor{
        .limit_low        = 0xFFFF,
        .base_low         = 0x0000,
        .base_mid         = 0x00,
        .access           = kAccessKernelCode,
        .flags_limit_high = static_cast<unsigned char>((kFlagsCode16 << 4) | 0x0F),
        .base_high        = 0x00};
}

/**
 * @brief   Build the six-entry boot GDT (null / code / data / long code /
 *          long data / 16-bit code).
 * @return  The full 48-byte table value.
 * @note    The table is born with its long mode pair and the 16-bit code
 *          entry so the single lgdt of the protected-mode switch admits
 *          every selector the boot chain ever loads.
 * @since   0.1.0
 * @ingroup boot_gdt
 */
constexpr BootGdt MakeBootGdt() {
    return BootGdt{.null   = SegmentDescriptor{},
                   .code   = MakeFlatDescriptor(kAccessKernelCode),
                   .data   = MakeFlatDescriptor(kAccessKernelData),
                   .code64 = MakeLongCodeDescriptor(),
                   .data64 = MakeLongDataDescriptor(),
                   .code16 = MakeCode16Descriptor()};
}

constexpr BootGdt kTemplate = MakeBootGdt();

inline constexpr unsigned long kFlatLimitBytes = (0xFFFFFUL * 0x1000UL) + 0xFFFUL;

}  // namespace cinux::boot::gdt
