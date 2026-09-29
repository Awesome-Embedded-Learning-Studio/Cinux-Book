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

#include <stddef.h>
#include <stdint.h>

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


using EmitFn = void (*)(char character, void* context);

/**
 * @brief         Cursor over a caller-owned output buffer.
 */
struct BufferSink {
    char*  data;      ///< First byte of the destination.
    size_t capacity;  ///< Bytes available through data.
    size_t position;  ///< Bytes written so far.
};

/**
 * @brief         Type-erased character sink.
 */
struct Sink {
    EmitFn emit;     ///< Receives every produced character.
    void*  context;  ///< Opaque state for emit.

    /**
     * @brief     Emits one character.
     */
    void put(char character) const { emit(character, context); }
};

/**
 * @brief         Parsed width/alignment/padding controls of one field.
 */
struct FieldSpec {
    int  width;       ///< Minimum field width in characters.
    bool left_align;  ///< Pad on the right instead of the left.
    bool zero_pad;    ///< Pad with '0' instead of ' '.
};

/**
 * @brief         Emits text honouring width/alignment/padding.
 */
inline void EmitPadded(Sink sink, TextView text, FieldSpec spec) {
    const char kPad = (spec.zero_pad && !spec.left_align) ? '0' : ' ';

    if (text.length >= spec.width) {
        for (int i = 0; i < text.length; ++i) {
            sink.put(text.data[i]);
        }
        return;
    }

    if (spec.left_align) {
        for (int i = 0; i < text.length; ++i) {
            sink.put(text.data[i]);
        }
        for (int i = text.length; i < spec.width; ++i) {
            sink.put(' ');
        }
        return;
    }

    for (int i = text.length; i < spec.width; ++i) {
        sink.put(kPad);
    }
    for (int i = 0; i < text.length; ++i) {
        sink.put(text.data[i]);
    }
}

/**
 * @brief         Parses width/flags ("-0NN") starting at cursor.
 *
 * @param[in,out] cursor   Advances past the parsed flags and digits.
 * @return        The parsed width/alignment/padding controls.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
inline FieldSpec ParseFieldSpec(const char*& cursor) {
    FieldSpec spec{.width = 0, .left_align = false, .zero_pad = false};

    while (*cursor == '-' || *cursor == '0') {
        if (*cursor == '-') {
            spec.left_align = true;
        } else {
            spec.zero_pad = true;
        }
        ++cursor;
    }

    while (*cursor >= '0' && *cursor <= '9') {
        spec.width = (spec.width * 10) + (*cursor - '0');
        ++cursor;
    }

    return spec;
}

/**
 * @brief         Parses the length modifier ("l"/"ll"/"z") at cursor.
 *
 * @param[in,out] cursor   Advances past the parsed modifier.
 * @return        0 for none, 1 for "l", 2 for "ll", 3 for "z".
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_format
 */
inline int ParseLengthModifier(const char*& cursor) {
    if (*cursor == 'l') {
        ++cursor;
        if (*cursor == 'l') {
            ++cursor;
            return 2;
        }
        return 1;
    }

    if (*cursor == 'z') {
        ++cursor;
        return 3;
    }

    return 0;
}

/**
 * @brief         The specifier loop shared by both entry points.
 *
 * @tparam        Source  Argument provider with next_char/next_string/
 *                        next_signed/next_unsigned/next_pointer members.
 */
template <typename Source>
void RunEngine(Sink sink, const char* format, Source& args) {
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            sink.put(*cursor);
            continue;
        }

        ++cursor;
        if (*cursor == '\0') {
            break;
        }

        FieldSpec spec    = ParseFieldSpec(cursor);
        const int kLength = ParseLengthModifier(cursor);

        const char kSpec = *cursor;
        if (kSpec == '\0') {
            break;
        }

        char             number_buffer[24];
        const CharBuffer kNumberOut{.data     = number_buffer,
                                    .capacity = static_cast<int>(sizeof(number_buffer))};
        TextView         text{};

        switch (kSpec) {
        case '%':
            sink.put('%');
            continue;

        case 'c':
            sink.put(args.next_char());
            continue;

        case 's':
            text = args.next_string();
            break;

        case 'd':
            text = FormatDecimal(args.next_signed(kLength), kNumberOut);
            break;

        case 'u':
        case 'o':
        case 'x':
        case 'X':
            text =
                FormatRadix(args.next_unsigned(kLength), RadixFor(kSpec), kSpec == 'X', kNumberOut);
            break;

        case 'p':
            text          = FormatPointerPrefixed(args.next_pointer(), kNumberOut);
            spec.zero_pad = false;
            break;

        default:
            sink.put('%');
            sink.put(kSpec);
            continue;
        }

        EmitPadded(sink, text, spec);
    }
}

}  // namespace cinux::base::format::detail
