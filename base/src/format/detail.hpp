/**
 * @file    detail.hpp
 * @brief   Private declarations shared between format-engine translation
 *          units. Never include from outside base/src/format/.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#pragma once

#include <cstdint>

namespace cinux::base::format::detail {

/**
 * @brief         A writable window over a caller-owned output buffer.
 *
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
struct CharBuffer {
    char* data;      ///< First byte of the destination.
    int   capacity;  ///< Bytes available through data.
};

/**
 * @brief         A read-only view of a character sequence and its length.
 *
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
struct TextView {
    const char* data;    ///< First character of the sequence.
    int         length;  ///< Characters in the sequence, excluding any NUL.
};

/**
 * @brief         Formats an unsigned integer in an arbitrary base.
 *
 * @param[in]     value        Value to convert.
 * @param[in]     base         Radix (2..16).
 * @param[in]     uppercase    Use upper-case hex digits.
 * @param[out]    out          Destination window (at least 24 bytes recommended).
 * @return        View of the characters written to out, excluding the NUL.
 * @note          Zero produces a single '0'.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
TextView FormatRadix(uint64_t value, unsigned base, bool uppercase, CharBuffer out);

/**
 * @brief         Formats a signed decimal integer.
 *
 * @param[in]     value        Value to convert.
 * @param[out]    out          Destination window (at least 24 bytes recommended).
 * @return        View of the characters written to out, excluding the NUL.
 * @note          INT64_MIN is special-cased: negating it overflows in
 *                two's complement, so the literal string is copied instead.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
TextView FormatDecimal(int64_t value, CharBuffer out);

/**
 * @brief         Maps a conversion specifier to its radix.
 *
 * @param[in]     spec         One of 'u', 'o', 'x', 'X'.
 * @return        10, 8 or 16 respectively; 10 for anything else.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
unsigned RadixFor(char spec);

/**
 * @brief         Formats a pointer as "0x" followed by lower-case hex.
 *
 * @param[in]     value        Pointer bits to render.
 * @param[out]    out          Destination window (at least 24 bytes recommended).
 * @return        View of the characters written to out, excluding the NUL.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
TextView FormatPointerPrefixed(uintptr_t value, CharBuffer out);

}  // namespace cinux::base::format::detail
