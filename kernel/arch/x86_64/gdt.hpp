/**
 * @file    gdt.hpp
 * @brief   The kernel's own three-slot GDT: null, kernel code, kernel data.
 *
 * Until this table exists the kernel executes on selectors borrowed from
 * the boot chain's table; loading this one is the moment the kernel owns
 * its own segment world. Three slots are deliberate — the task-state and
 * user segments join when user mode gives them a motive, not before. The
 * descriptor encoding is re-checked in the host world by test_gdt.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::arch::gdt {

struct [[gnu::packed]] SegmentDescriptor {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  flags_limit_high;
    unsigned char  base_high;
};

/// Access-byte grammar of a code/data descriptor; two vocabularies that
/// must never mix, hence two scoped enums instead of raw bytes.
enum class SegmentAccess : unsigned char {
    kPresent    = 1U << 7,  ///< Bit 7: the segment exists.
    kRing0      = 0U << 5,  ///< Bits 6..5: descriptor privilege level, kernel.
    kCodeData   = 1U << 4,  ///< Bit 4: a code or data segment, not a system one.
    kExecutable = 1U << 3,  ///< Bit 3: executable — a code segment.
    kReadWrite  = 1U << 1,  ///< Bit 1: readable for code, writable for data.
};

/// Flag-nibble grammar: the top nibble of flags_limit_high, bits 3..0.
enum class SegmentFlags : unsigned char {
    kGranularity4K = 1U << 3,  ///< Nibble bit 3: limit counts in 4K units.
    kLongMode      = 1U << 1,  ///< Nibble bit 1: L=1, the 64-bit default size.
};

/**
 * @brief         Union of two access-byte vocabularies.
 * @param[in]     left   First access mask.
 * @param[in]     right  Second access mask.
 * @return        The combined access byte vocabulary.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] constexpr SegmentAccess operator|(SegmentAccess left, SegmentAccess right) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<SegmentAccess>(static_cast<unsigned char>(left) |
                                      static_cast<unsigned char>(right));
}

/**
 * @brief         Union of two flag-nibble vocabularies.
 * @param[in]     left   First flags mask.
 * @param[in]     right  Second flags mask.
 * @return        The combined flags nibble vocabulary.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
[[nodiscard]] constexpr SegmentFlags operator|(SegmentFlags left, SegmentFlags right) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<SegmentFlags>(static_cast<unsigned char>(left) |
                                     static_cast<unsigned char>(right));
}

/**
 * @brief         Place a flags nibble above the limit's high nibble.
 * @param[in]     flags  The flags vocabulary to shift in.
 * @return        The combined flags_limit_high byte.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr unsigned char MakeFlagsLimitHigh(SegmentFlags flags) {
    return static_cast<unsigned char>((static_cast<unsigned char>(flags) << 4) |
                                      cinux::base::bit::Ones<unsigned char>(4));
}

/// Selector of the second entry: the kernel code segment.
inline constexpr unsigned short kSelectorCode = 0x08;

/// Selector of the third entry: the kernel data segment.
inline constexpr unsigned short kSelectorData = 0x10;

/**
 * @brief         Build the long-mode code descriptor: base 0, L=1, D=0.
 * @return        The encoded 8-byte descriptor (0x00AF9A000000FFFF).
 * @note          L=1 with D=0 is the hard architectural pairing for 64-bit
 *                code segments; test_gdt pins both bits.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr SegmentDescriptor MakeLongCodeDescriptor() {
    return SegmentDescriptor{
        .limit_low = 0xFFFF,
        .base_low  = 0x0000,
        .base_mid  = 0x00,
        .access = static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing0 |
                                             SegmentAccess::kCodeData | SegmentAccess::kExecutable |
                                             SegmentAccess::kReadWrite),
        .flags_limit_high =
            MakeFlagsLimitHigh(SegmentFlags::kGranularity4K | SegmentFlags::kLongMode),
        .base_high = 0x00};
}

/**
 * @brief         Build the long-mode data descriptor: base 0, D=0.
 * @return        The encoded 8-byte descriptor (0x008F92000000FFFF).
 * @note          Long mode ignores base and limit on data segments; the
 *                entry exists so the selector arithmetic stays uniform.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr SegmentDescriptor MakeLongDataDescriptor() {
    return SegmentDescriptor{
        .limit_low = 0xFFFF,
        .base_low  = 0x0000,
        .base_mid  = 0x00,
        .access = static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing0 |
                                             SegmentAccess::kCodeData | SegmentAccess::kReadWrite),
        .flags_limit_high = MakeFlagsLimitHigh(SegmentFlags::kGranularity4K),
        .base_high        = 0x00};
}

struct [[gnu::packed]] KernelGdt {
    SegmentDescriptor null;
    SegmentDescriptor code;
    SegmentDescriptor data;
};

/**
 * @brief         Build the kernel's three-entry GDT value.
 * @return        Null descriptor plus the long-mode code/data pair.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr KernelGdt MakeKernelGdt() {
    return KernelGdt{.null = SegmentDescriptor{},
                     .code = MakeLongCodeDescriptor(),
                     .data = MakeLongDataDescriptor()};
}

struct [[gnu::packed]] TablePointer {
    unsigned short     limit;
    unsigned long long base;
};

/**
 * @brief         Load the kernel GDT and start running on its selectors.
 * @return        None.
 * @note          lgdt followed by a far return into selector 0x08 and a
 *                reload of every data segment register; interrupts are
 *                still off at this point in the bring-up, so the reload
 *                races nothing.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void LoadOwnedGdt();

}  // namespace cinux::arch::gdt
