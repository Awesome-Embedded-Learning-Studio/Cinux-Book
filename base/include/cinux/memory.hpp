/**
 * @file    memory.hpp
 * @brief   Byte-level copy and fill over raw memory.
 *
 * Shared wheels where hand-rolled loops would otherwise grow in every
 * world: host tests, the kernel, the boot chain, and anything else that
 * includes base. Header-inline on purpose — each world instantiates
 * them for free and no translation unit owns state.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_memory
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base {

/**
 * @brief         Copies bytes from one raw region to another.
 *
 * @param[in,out] destination   Where the bytes land.
 * @param[in]     source        Where the bytes come from.
 * @param[in]     bytes         How many bytes to copy.
 * @return        None
 * @note          Regions must not overlap; this is the memcpy shape,
 *                not memmove.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_memory
 */
inline void CopyBytes(void* destination, const void* source, unsigned long bytes) {
    // NOLINTNEXTLINE(misc-const-correctness)
    auto*       dest_byte = static_cast<unsigned char*>(destination);
    auto const* src_byte  = static_cast<const unsigned char*>(source);
    while (bytes-- > 0) {
        // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
        *dest_byte++ = *src_byte++;
    }
}

/**
 * @brief         Fills a raw region with one byte value.
 *
 * @param[in,out] destination   Where the fill lands.
 * @param[in]     value         The byte to repeat.
 * @param[in]     bytes         How many bytes to fill.
 * @return        None
 * @note          This is the memset shape; zeroing a table page is its
 *                everyday caller.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_memory
 */
inline void SetBytes(void* destination, unsigned char value, unsigned long bytes) {
    // NOLINTNEXTLINE(misc-const-correctness)
    auto* dest_byte = static_cast<unsigned char*>(destination);
    while (bytes-- > 0) {
        // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
        *dest_byte++ = value;
    }
}

/**
 * @brief         Asks whether a raw region holds one byte value throughout.
 *
 * @param[in]     area          The region to inspect.
 * @param[in]     value         The byte every position must hold.
 * @param[in]     bytes         How many bytes to inspect.
 * @return        True when every inspected byte equals value.
 * @note          The zero-value question is the everyday caller: is this
 *                block clean, is that gap truly zeroed, did the fill
 *                reach the end.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_memory
 */
[[nodiscard]] inline bool BytesAre(const void* area, unsigned char value, unsigned long bytes) {
    auto const* scan_byte = static_cast<const unsigned char*>(area);
    while (bytes-- > 0) {
        if (*scan_byte++ != value) {
            return false;
        }
    }
    return true;
}

/**
 * @brief         Asks whether two raw regions hold the same bytes.
 *
 * @param[in]     left          One region.
 * @param[in]     right         The other region.
 * @param[in]     bytes         How many bytes to compare.
 * @return        True when the regions agree byte for byte.
 * @note          The memcmp-equals shape; ordering comparisons stay
 *                with their callers until one exists.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       base_memory
 */
[[nodiscard]] inline bool EqualBytes(const void* left, const void* right, unsigned long bytes) {
    auto const* left_byte  = static_cast<const unsigned char*>(left);
    auto const* right_byte = static_cast<const unsigned char*>(right);
    while (bytes-- > 0) {
        if (*left_byte++ != *right_byte++) {
            return false;
        }
    }
    return true;
}

/**
 * @brief         Counts one NUL-terminated string's bytes, bounded.
 * @param[in]     text   The string to measure; the scan never runs past
 *                       the bound even when no NUL waits there.
 * @param[in]     cap    How many bytes the scan may touch, one upward.
 * @return        The bytes before the NUL, capped by the bound.
 * @note          The strlen-equals shape with a wall; the format
 *                engine answers void, so its callers measure what it
 *                wrote with this instead of trusting luck.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       base_memory
 */
[[nodiscard]] inline unsigned long StrLength(const char* text, unsigned long cap) {
    unsigned long length = 0;
    while (length + 1 < cap && text[length] != '\0') {
        ++length;
    }
    return length;
}

}  // namespace cinux::base
