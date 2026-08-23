/**
 * @file detail/vformat.hpp
 * @brief Hardware-agnostic formatting engine — internal, not public API.
 *
 * Migrated from kernel/lib/private/vkprintf_impl.hpp.
 * The template version is kept in the header; a buffer-writing wrapper
 * is provided in src/detail/vformat.cpp for Logger consumption.
 *
 * Supported: %% %c %s %d %u %x %X %p
 * Length:    %ld %lu %lx %lX %lld %llu %llx %llX
 * Width:     %Nd %0Nd %-Nd %-Ns
 */

#ifndef CINUX_DETAIL_VFORMAT_HPP
#define CINUX_DETAIL_VFORMAT_HPP

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace cinux::lib::detail {

// ============================================================
// Number formatting helpers
// ============================================================

/** @brief Format signed 64-bit integer as decimal. Returns chars written. */
inline int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) {
        return 0;
    }

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == static_cast<int64_t>(0x8000000000000000ULL)) {
            const char* min_str = "-9223372036854775808";
            int         len     = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1) {
                buffer[idx++] = min_str[len++];
            }
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';
    return idx;
}

/** @brief Format unsigned 64-bit integer as decimal. Returns chars written. */
inline int format_unsigned(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) {
        return 0;
    }

    char tmp[24];
    int  tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(value % 10);
        value /= 10;
    } while (value > 0 && tmp_idx < 24);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';
    return idx;
}

/** @brief Format unsigned 64-bit integer as hex. Returns chars written. */
inline int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) {
        return 0;
    }

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char        tmp[20];
    int         tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';
    return idx;
}

// ============================================================
// Generic formatted output engine (template)
// ============================================================

/** @brief OutputFn: callable(char) — invoked for each output character. */
template <typename OutputFn>
void vformat_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }
        fmt++;

        // Parse flags
        bool left_align = false;
        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }

        bool zero_pad = false;
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // Parse width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Parse length modifier
        int long_count = 0;
        while (*fmt == 'l') {
            long_count++;
            fmt++;
        }

        char type = *fmt++;
        int  len  = 0;

        switch (type) {
        case '%':
            putc_fn('%');
            break;
        case 'c':
            putc_fn(static_cast<char>(va_arg(args, int)));
            break;

        case 's': {
            const char* s = va_arg(args, const char*);
            if (!s) {
                s = "(null)";
            }
            int slen = 0;
            while (s[slen]) {
                slen++;
            }

            if (left_align) {
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
            } else {
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
            }
            break;
        }

        case 'd': {
            int64_t dv =
                (long_count > 0) ? va_arg(args, int64_t) : static_cast<int64_t>(va_arg(args, int));
            len             = format_decimal(dv, buffer, sizeof(buffer));
            bool has_sign   = (len > 0 && buffer[0] == '-');
            int  digits_len = has_sign ? len - 1 : len;

            if (!left_align && zero_pad && has_sign) {
                putc_fn('-');
                for (int i = digits_len; i < width - 1; i++) {
                    putc_fn('0');
                }
                for (int i = 1; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else if (!left_align) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else {
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'u': {
            uint64_t uv = (long_count > 0) ? va_arg(args, uint64_t)
                                           : static_cast<uint64_t>(va_arg(args, unsigned));
            len         = format_unsigned(uv, buffer, sizeof(buffer));
            char pad    = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'x': {
            uint64_t xv = (long_count > 0) ? va_arg(args, uint64_t)
                                           : static_cast<uint64_t>(va_arg(args, unsigned));
            len         = format_hex(xv, buffer, sizeof(buffer), true);
            char pad    = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'X': {
            uint64_t Xv = (long_count > 0) ? va_arg(args, uint64_t)
                                           : static_cast<uint64_t>(va_arg(args, unsigned));
            len         = format_hex(Xv, buffer, sizeof(buffer), false);
            char pad    = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }

        case 'p': {
            putc_fn('0');
            putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            for (int i = len; i < 16; i++) {
                putc_fn('0');
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;
        }

        default:
            putc_fn('%');
            putc_fn(type);
            break;
        }
    }
}

// ============================================================
// Buffer-writing wrapper (implemented in vformat.cpp)
// ============================================================

/** @brief Format into a fixed-size buffer. Null-terminates the output. */
void vformat_to_buf(char* buf, size_t buf_size, const char* fmt, va_list args);

}  // namespace cinux::lib::detail

#endif  // CINUX_DETAIL_VFORMAT_HPP
