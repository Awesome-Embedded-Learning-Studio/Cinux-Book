/**
 * @file    entry.cpp
 * @brief   Public entry point and private parser core of the format engine.
 *
 * The engine is type-erased: it hands each produced character to a plain
 * function pointer plus context, so nothing implementation-related leaks
 * into the public include tree and any sink (buffer, serial, test spy)
 * can drive it.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#include <cinux/format.hpp>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "detail.hpp"

namespace cinux::base::format {

namespace {

using EmitFn = void (*)(char character, void* context);

struct BufferSink {
    char*  data;
    size_t capacity;
    size_t position;
};

void buffer_emit(char character, void* context) {
    auto* sink = static_cast<BufferSink*>(context);
    if (sink->position + 1 < sink->capacity) {
        sink->data[sink->position] = character;
        ++sink->position;
    }
}

struct Sink {
    EmitFn emit;
    void*  context;

    void put(char character) const { emit(character, context); }
};

struct FieldSpec {
    int  width;
    bool left_align;
    bool zero_pad;
};

void emit_padded(Sink sink, detail::TextView text, FieldSpec spec) {
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

int64_t read_signed_arg(va_list args, int length) {
    if (length == 0) {
        return va_arg(args, int);
    }
    return va_arg(args, long long);
}

uint64_t read_unsigned_arg(va_list args, int length) {
    if (length == 0) {
        return va_arg(args, unsigned int);
    }
    return va_arg(args, unsigned long long);
}

detail::TextView read_string_arg(va_list args) {
    const char* source = va_arg(args, const char*);
    if (source == nullptr) {
        source = "(null)";
    }
    return {.data = source, .length = static_cast<int>(strlen(source))};
}

FieldSpec parse_field_spec(const char*& cursor) {
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

int parse_length_modifier(const char*& cursor) {
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

void run_engine(Sink sink, const char* format, va_list args) {
    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            sink.put(*cursor);
            continue;
        }

        ++cursor;
        if (*cursor == '\0') {
            break;
        }

        FieldSpec spec    = parse_field_spec(cursor);
        const int kLength = parse_length_modifier(cursor);

        const char kSpec = *cursor;
        if (kSpec == '\0') {
            break;
        }

        char                     number_buffer[24];
        const detail::CharBuffer kNumberOut{.data     = number_buffer,
                                            .capacity = static_cast<int>(sizeof(number_buffer))};
        detail::TextView         text{};

        switch (kSpec) {
        case '%':
            sink.put('%');
            continue;

        case 'c':
            sink.put(static_cast<char>(va_arg(args, int)));
            continue;

        case 's':
            text = read_string_arg(args);
            break;

        case 'd':
            text = detail::FormatDecimal(read_signed_arg(args, kLength), kNumberOut);
            break;

        case 'u':
        case 'o':
        case 'x':
        case 'X':
            text = detail::FormatRadix(read_unsigned_arg(args, kLength), detail::RadixFor(kSpec),
                                       kSpec == 'X', kNumberOut);
            break;

        case 'p':
            text = detail::FormatPointerPrefixed(reinterpret_cast<uintptr_t>(va_arg(args, void*)),
                                                 kNumberOut);
            spec.zero_pad = false;
            break;

        default:
            sink.put('%');
            sink.put(kSpec);
            continue;
        }

        emit_padded(sink, text, spec);
    }
}

}  // namespace

void VformatToBuf(char* out_buffer, size_t buffer_size, const char* format, ...) {
    if (out_buffer == nullptr || buffer_size == 0) {
        return;
    }

    va_list args;
    va_start(args, format);

    BufferSink buffer_sink{.data = out_buffer, .capacity = buffer_size, .position = 0};
    const Sink kSink{.emit = &buffer_emit, .context = &buffer_sink};
    run_engine(kSink, format, args);

    va_end(args);
    out_buffer[buffer_sink.position] = '\0';
}

}  // namespace cinux::base::format
