/**
 * @file    gdt.hpp
 * @brief   The kernel's own GDT: kernel pair, TSS pair, user pair.
 *
 * Until this table exists the kernel executes on selectors borrowed from
 * the boot chain's table; loading this one is the moment the kernel owns
 * its own segment world. The table grew with its motives: three slots
 * (null, kernel code, kernel data) served the kernel-only era, and the
 * user-mode station added the task-state pair and the user code/data
 * pair. The SYSRET selector arithmetic is baked in from day one: the
 * STAR base already carries RPL 3 (0x23, not 0x20), so SYSRETQ produces
 * 0x33/0x2B no matter whether the CPU sets SS.RPL itself. The
 * descriptor encoding is re-checked in the host world by test_gdt.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>

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
    kRing3      = 3U << 5,  ///< Bits 6..5: descriptor privilege level, user.
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

/// Selector of the task-state pair that follows the kernel pair.
inline constexpr unsigned short kSelectorTss = 0x18;

/// STAR[63:48] base for SYSRETQ, RPL 3 folded into the base itself.
inline constexpr unsigned short kSysretStarBase = 0x23;

/// Selector of the user data segment: SYSRETQ computes base + 8.
inline constexpr unsigned short kSelectorUserData = kSysretStarBase + 8;

/// Selector of the user code segment: SYSRETQ computes base + 16.
inline constexpr unsigned short kSelectorUserCode = kSysretStarBase + 16;

static_assert(kSelectorUserData == 0x2B, "SYSRETQ user data lands at 0x2B");
static_assert(kSelectorUserCode == 0x33, "SYSRETQ user code lands at 0x33");
static_assert((kSelectorUserData >> 3) == 5, "user data is slot five");
static_assert((kSelectorUserCode >> 3) == 6, "user code is slot six");

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

/**
 * @brief         Build the long-mode user code descriptor: DPL 3, L=1.
 * @return        The encoded 8-byte descriptor (0x00AFFA000000FFFF).
 * @note          Same shape as the kernel code descriptor with the
 *                privilege bits raised; selectors arrive via the STAR
 *                arithmetic, see kSysretStarBase.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
constexpr SegmentDescriptor MakeUserCodeDescriptor() {
    return SegmentDescriptor{
        .limit_low = 0xFFFF,
        .base_low  = 0x0000,
        .base_mid  = 0x00,
        .access = static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing3 |
                                             SegmentAccess::kCodeData | SegmentAccess::kExecutable |
                                             SegmentAccess::kReadWrite),
        .flags_limit_high =
            MakeFlagsLimitHigh(SegmentFlags::kGranularity4K | SegmentFlags::kLongMode),
        .base_high = 0x00};
}

/**
 * @brief         Build the long-mode user data descriptor: DPL 3, D=0.
 * @return        The encoded 8-byte descriptor (0x008FF2000000FFFF).
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
constexpr SegmentDescriptor MakeUserDataDescriptor() {
    return SegmentDescriptor{
        .limit_low = 0xFFFF,
        .base_low  = 0x0000,
        .base_mid  = 0x00,
        .access = static_cast<unsigned char>(SegmentAccess::kPresent | SegmentAccess::kRing3 |
                                             SegmentAccess::kCodeData | SegmentAccess::kReadWrite),
        .flags_limit_high = MakeFlagsLimitHigh(SegmentFlags::kGranularity4K),
        .base_high        = 0x00};
}

/// A 64-bit task-state descriptor spans two slots: the familiar shape
/// plus a second quadword holding base[63:32] and reserved zeros.
struct [[gnu::packed]] TssDescriptor {
    SegmentDescriptor low;        ///< Limit, base[31:0], access for a TSS.
    unsigned int      base_high;  ///< Base[63:32] of the task struct.
    unsigned int      reserved;   ///< Must stay zero in long mode.
};

static_assert(sizeof(TssDescriptor) == 16, "a 64-bit TSS descriptor is 16 bytes");

/**
 * @brief         Encode a 64-bit available-TSS descriptor pair.
 * @param[in]     base   Address of the task struct.
 * @param[in]     limit  One past the last byte, minus one (byte units).
 * @return        The 16-byte descriptor as the CPU wants it.
 * @note          Access byte is 0x89: present, ring 0, system type,
 *                available 64-bit TSS. Granularity stays byte-unit —
 *                a TSS is 104 bytes, far below any 4K page boundary.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
constexpr TssDescriptor MakeTssDescriptor(unsigned long long base, unsigned int limit) {
    return TssDescriptor{
        .low =
            SegmentDescriptor{.limit_low        = static_cast<unsigned short>(limit & 0xFFFF),
                              .base_low         = static_cast<unsigned short>(base & 0xFFFF),
                              .base_mid         = static_cast<unsigned char>((base >> 16) & 0xFF),
                              .access           = static_cast<unsigned char>(0x89),
                              .flags_limit_high = static_cast<unsigned char>((limit >> 16) & 0x0F),
                              .base_high        = static_cast<unsigned char>((base >> 24) & 0xFF)},
        .base_high = static_cast<unsigned int>((base >> 32) & 0xFFFFFFFFULL),
        .reserved  = 0};
}

struct [[gnu::packed]] KernelGdt {
    SegmentDescriptor null;
    SegmentDescriptor code;
    SegmentDescriptor data;
    SegmentDescriptor tss_low;    ///< Filled at load time with the real base.
    unsigned int      tss_upper;  ///< Base[63:32] of the task struct.
    unsigned int      tss_zero;   ///< Reserved quadword half, stays zero.
    SegmentDescriptor user_data;
    SegmentDescriptor user_code;
};

static_assert(offsetof(KernelGdt, tss_low) == 24, "the TSS pair owns slots three and four");
static_assert(offsetof(KernelGdt, user_data) == 40, "user data is slot five");
static_assert(offsetof(KernelGdt, user_code) == 48, "user code is slot six");

/**
 * @brief         Build the kernel's seven-entry GDT value.
 * @return        Null, kernel code/data, an empty TSS pair, user
 *                code/data. The TSS base is zero here on purpose:
 *                it names a runtime address, so LoadOwnedGdt installs
 *                it when the real task struct exists.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
constexpr KernelGdt MakeKernelGdt() {
    return KernelGdt{.null      = SegmentDescriptor{},
                     .code      = MakeLongCodeDescriptor(),
                     .data      = MakeLongDataDescriptor(),
                     .tss_low   = SegmentDescriptor{},
                     .tss_upper = 0,
                     .tss_zero  = 0,
                     .user_data = MakeUserDataDescriptor(),
                     .user_code = MakeUserCodeDescriptor()};
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
 *                races nothing. The TSS pair in the static table is
 *                zero from MakeKernelGdt; this call encodes the real
 *                address and limit of the one Tss before loading.
 *                Loading the register itself (ltr) belongs to the
 *                task-state side, tss.hpp.
 * @since         0.1.0
 * @ingroup       kernel_arch
 */
void LoadOwnedGdt();

}  // namespace cinux::arch::gdt
