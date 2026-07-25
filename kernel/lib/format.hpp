/**
 * @file kernel/lib/format.hpp
 * @brief Lightweight C++17 type-safe format ({fmt}-style {} placeholders)
 *
 * Replaces both the old LineBuilder `b.put_s / b.put_u / b.put` eyesore (six
 * calls to spell one line) and libc-style `va_arg` snprintf (not type-safe).
 * Variadic templates + `if constexpr` give compile-time type dispatch; `{}`
 * placeholders match {fmt}/std::format spelling.
 *
 * Supported argument types (auto-dispatched):
 *   - `const char*` / `char*`  -> C string (null -> "(null)")
 *   - `char`                   -> single character
 *   - any integral type        -> decimal (signed gets a leading '-')
 * Unsupported types fail at compile time via static_assert.
 *
 * Usage:
 *   cinux::fmt::format(buf, cap, "processor\t: {}\n", cpu_id);
 *   cinux::fmt::format(p, rest, "model name\t: {} {}\n", kCpuModel, kOsVersion);
 *
 * Header-only, freestanding-clean: only <type_traits> from the DIRECTIVES-A
 * allow-list, plus <cstdint>. No <stdarg>, no STL containers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <type_traits>  // is_integral_v / is_signed_v / is_same_v (DIRECTIVES A)
#include <utility>      // std::forward (DIRECTIVES A)

namespace cinux::fmt {

/// Bounded output cursor; push() always leaves room for a trailing NUL.
struct Sink {
    char*    buf;
    uint32_t cap;
    uint32_t len{0};

    void push(char c) {
        if (len + 1 < cap) {
            buf[len] = c;
        }
        ++len;
    }
    /// NUL-terminate at the write cursor (no-op if cap == 0).
    void finish() {
        if (cap > 0) {
            buf[len < cap ? len : cap - 1] = '\0';
        }
    }
};

/// Per-argument emitter. `if constexpr` dispatches by decayed type so any
/// integral width (uint8_t apic_id / uint32_t pid / int counter ...) just works.
template <typename T>
void emit(Sink& s, const T& value) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        const char* str = value;  // copy the pointer out of the reference first;
        if (str == nullptr) {     // comparing the reference itself to nullptr trips -Wnonnull-compare
            str = "(null)";
        }
        while (*str != '\0') {
            s.push(*str++);
        }
    } else if constexpr (std::is_same_v<U, char>) {
        s.push(value);
    } else if constexpr (std::is_integral_v<U>) {
        unsigned long long v = static_cast<unsigned long long>(value);
        if constexpr (std::is_signed_v<U>) {
            if (value < 0) {
                s.push('-');
                v = 0ULL - static_cast<unsigned long long>(value);
            }
        }
        char tmp[20];  // ull max = 20 digits
        int  n = 0;
        if (v == 0) {
            tmp[n++] = '0';
        }
        while (v > 0) {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        for (int i = n - 1; i >= 0; --i) {
            s.push(tmp[i]);
        }
    } else {
        static_assert(!sizeof(T),
                      "cinux::fmt::format: unsupported argument type "
                      "(add an emit() overload or cast at the call site)");
    }
}

/// Base case: no more arguments -- copy the rest of @p fmt verbatim.
inline void run(Sink& s, const char* fmt) {
    while (*fmt != '\0') {
        s.push(*fmt++);
    }
}

/// Consume one `{}` per argument via overload-resolved emit(), then recurse.
template <typename T, typename... Rest>
void run(Sink& s, const char* fmt, T&& first, Rest&&... rest) {
    while (*fmt != '\0') {
        if (fmt[0] == '{' && fmt[1] == '}') {
            emit(s, std::forward<T>(first));
            run(s, fmt + 2, std::forward<Rest>(rest)...);
            return;
        }
        s.push(*fmt++);
    }
    // More arguments than placeholders: trailing args silently ignored. Flip
    // to a kpanic if a DEBUG-format path ever wants the diagnostic.
}

/// Format with `{}` placeholders into @p buf (always NUL-terminated, truncated
/// to @p cap-1 chars).  @return characters written (excl. NUL); == cap-1 truncated.
template <typename... Args>
uint32_t format(char* buf, uint32_t cap, const char* fmt, Args&&... args) {
    if (cap == 0) {
        return 0;
    }
    Sink s{buf, cap, 0};
    run(s, fmt, std::forward<Args>(args)...);
    s.finish();
    return s.len < s.cap ? s.len : s.cap - 1;
}

}  // namespace cinux::fmt
