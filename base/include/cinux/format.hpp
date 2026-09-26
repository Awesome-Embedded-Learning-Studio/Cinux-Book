/**
 * @file    format.hpp
 * @brief   Buffer-formatting entry points (printf-style, allocation-free).
 *
 * Public surface of the Cinux format engine. The entry point and parsing
 * core live in base/src/format/entry.cpp, number conversion in
 * base/src/format/radix.cpp, and the declarations they share in
 * base/src/format/detail.hpp. Supported specifiers:
 *
 *   %%  %c  %s  %d  %u  %o  %x  %X  %p
 *   length: %l / %ll / %z      width: %Nd  %0Nd  %-Nd  %-Ns
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#pragma once

#include <cstddef>

namespace cinux::base::format {

/**
 * @brief         Formats into a fixed buffer (truncating, never overflowing).
 *
 * Writes at most buffer_size - 1 characters and always NUL-terminates,
 * so the strict `pos + 1 < buffer_size` bound keeps room for the NUL.
 *
 * @param[in]     out_buffer   Destination buffer (may be null, call is a no-op).
 * @param[in]     buffer_size  Capacity of the destination buffer, in bytes.
 * @param[in]     format       printf-style format string.
 * @param[in]     ...          Format arguments.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
void VformatToBuf(char* out_buffer, size_t buffer_size, const char* format, ...);

}  // namespace cinux::base::format
