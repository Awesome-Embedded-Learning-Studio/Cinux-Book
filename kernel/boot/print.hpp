/**
 * @file    print.hpp
 * @brief   Format-and-emit sugar over the base format engine.
 *
 * Print and Println are the C++-variadic face of FormatToBuf: they build
 * the type-erased argument array at compile time, which keeps the plain
 * struct calling convention that survives -m16. Only C variadic slots are
 * banned in that world — templates are not.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_boot
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/format.hpp"
#include "kernel/console/console.hpp"

namespace cinux::print {

/** @brief Stack buffer behind every Print/Println call. */
inline constexpr unsigned kPrintBufferSize = 192;

namespace detail {

/**
 * @brief         Wraps an integer argument for the format engine.
 *
 * @param[in]     value   Integer to carry.
 * @return        The wrapped argument.
 * @note          Every integral type funnels here; unsigned long long is
 *                the single viable numeric overload.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr base::format::Arg Argify(unsigned long long value) {
    return base::format::NumU(value);
}

/**
 * @brief         Wraps a string argument for the format engine.
 *
 * @param[in]     value   String to carry.
 * @return        The wrapped argument.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr base::format::Arg Argify(const char* value) {
    return base::format::Str(value);
}

/**
 * @brief         Wraps a pointer argument for the format engine.
 *
 * @param[in]     value   Pointer to carry.
 * @return        The wrapped argument.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
constexpr base::format::Arg Argify(const void* value) {
    return base::format::Ptr(value);
}

/**
 * @brief         Emits a format string with no arguments.
 *
 * @param[in]     newline   Append a trailing newline when true.
 * @param[in]     fmt       printf-style format string.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
inline void Emit(bool newline, const char* fmt) {
    char                         buf[kPrintBufferSize];
    base::format::ArgsView const kNone{.data = nullptr, .count = 0};
    base::format::FormatToBuf(buf, sizeof(buf), fmt, kNone);
    console::PutString(buf);
    if (newline) {
        console::PutChar('\n');
    }
}

/**
 * @brief         Emits a formatted line through the base format engine.
 *
 * @param[in]     newline   Append a trailing newline when true.
 * @param[in]     fmt       printf-style format string.
 * @param[in]     args      Arguments to interpolate, one per specifier.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
template <typename... Args>
void Emit(bool newline, const char* fmt, Args... args) {
    char                    buf[kPrintBufferSize];
    // NOLINTNEXTLINE(readability-identifier-naming)
    base::format::Arg const kArgs[] = {Argify(args)...};
    base::format::FormatToBuf(buf, sizeof(buf), fmt, base::format::ArgsView::of(kArgs));
    console::PutString(buf);
    if (newline) {
        console::PutChar('\n');
    }
}

}  // namespace detail

/**
 * @brief         Writes a formatted string to the debug console.
 *
 * @param[in]     fmt     printf-style format string.
 * @param[in]     args    Arguments to interpolate.
 * @return        None
 * @note          Truncates at kPrintBufferSize, never overflows.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
template <typename... Args>
void Print(const char* fmt, Args... args) {
    detail::Emit(false, fmt, args...);
}

/**
 * @brief         Writes a formatted string plus a newline.
 *
 * @param[in]     fmt     printf-style format string.
 * @param[in]     args    Arguments to interpolate.
 * @return        None
 * @note          Truncates at kPrintBufferSize, never overflows.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_boot
 */
template <typename... Args>
void Println(const char* fmt, Args... args) {
    detail::Emit(true, fmt, args...);
}

}  // namespace cinux::print
