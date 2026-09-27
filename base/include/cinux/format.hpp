/**
 * @file    format.hpp
 * @brief   Buffer-formatting entry points (printf-style, allocation-free).
 *
 * Public surface of the Cinux format engine. The every-world entry point
 * and parsing core live in base/src/format/entry.cpp, the host-only
 * variadic sugar in base/src/format/entry_variadic.cpp, number conversion
 * in base/src/format/radix.cpp, and the declarations they share in
 * base/src/format/detail.hpp. Supported specifiers:
 *
 *   %%  %c  %s  %d  %u  %o  %x  %X  %p
 *   length: %l / %ll / %z      width: %Nd  %0Nd  %-Nd  %-Ns
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_format
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::base::format {

/**
 * @brief         One type-erased formatting argument.
 *
 * Carries the value as raw bits; the specifier decides how to read it,
 * exactly like a printf argument — but as a plain struct it passes
 * through the -m16 world's calling convention unharmed, where C
 * variadic slots do not.
 */
struct Arg {
    /**
     * @brief     How one argument was intended to be read.
     */
    enum class Kind : uint8_t {
        kUnsigned,  ///< Read as an unsigned integer.
        kSigned,    ///< Read as a signed integer.
        kString,    ///< Read as a NUL-terminated string pointer.
        kPointer,   ///< Read as a pointer.
    };

    Kind     kind;   ///< How the value was intended to be read.
    uint64_t value;  ///< Raw bits: integer value or pointer address.
};

/**
 * @brief     Wraps an unsigned integer argument.
 */
/**
 * @brief     Wraps an unsigned integer argument.
 *
 * @param[in] raw   Value to carry.
 * @return    The wrapped argument.
 */
constexpr Arg NumU(uint64_t raw) {
    return {.kind = Arg::Kind::kUnsigned, .value = raw};
}
/**
 * @brief     Wraps a signed integer argument.
 *
 * @param[in] raw   Value to carry.
 * @return    The wrapped argument.
 */
constexpr Arg NumS(int64_t raw) {
    return {.kind = Arg::Kind::kSigned, .value = static_cast<uint64_t>(raw)};
}
/**
 * @brief     Wraps a string argument.
 *
 * @param[in] raw   String to carry.
 * @return    The wrapped argument.
 */
constexpr Arg Str(const char* raw) {
    return {.kind = Arg::Kind::kString, .value = reinterpret_cast<uint64_t>(raw)};
}
/**
 * @brief     Wraps a pointer argument.
 *
 * @param[in] raw   Pointer to carry.
 * @return    The wrapped argument.
 */
constexpr Arg Ptr(const void* raw) {
    return {.kind = Arg::Kind::kPointer, .value = reinterpret_cast<uint64_t>(raw)};
}

/**
 * @brief         A borrowed array of format arguments.
 */
struct ArgsView {
    const Arg* data;   ///< First argument.
    size_t     count;  ///< Number of arguments.

    /**
     * @brief     Builds a view over a stack array, counting it.
     */
    template <size_t Count>
    static constexpr ArgsView of(const Arg (&array)[Count]) {
        return {.data = array, .count = Count};
    }
};

/**
 * @brief         Formats into a fixed buffer from a type-erased argument array.
 *
 * Same specifiers as the variadic entry point; arguments are consumed in
 * array order. Safe for every compilation world (host, -m16 boot), because
 * plain struct passing keeps one calling convention everywhere.
 *
 * @param[in]     out_buffer   Destination buffer (may be null, call is a no-op).
 * @param[in]     buffer_size  Capacity of the destination buffer, in bytes.
 * @param[in]     format       printf-style format string.
 * @param[in]     args         Arguments in consumption order (see ArgsView).
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
void FormatToBuf(char* out_buffer, size_t buffer_size, const char* format, ArgsView args);

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
 * @note          Host world only: C variadic slots disagree between caller
 *                and callee under -m16, so -m16 code must call FormatToBuf.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
void VformatToBuf(char* out_buffer, size_t buffer_size, const char* format, ...);

}  // namespace cinux::base::format
