/**
 * @file    entry_variadic.cpp
 * @brief   Host-only variadic sugar over the format engine.
 *
 * VformatToBuf converts C variadic arguments into an Arg array walk. It must
 * never be linked into -m16 code: under -m16 the caller pushes variadic
 * slots the callee's va_arg walk cannot agree with, so boot worlds call
 * FormatToBuf (entry.cpp) instead. File-level separation keeps the variadic
 * code out of boot images by construction.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cinux/format.hpp>

#include "detail.hpp"

namespace cinux::base::format {

namespace {

class VaSource {
public:
    explicit VaSource(va_list args) { va_copy(args_, args); }
    ~VaSource() { va_end(args_); }

    VaSource(const VaSource&)            = delete;
    VaSource& operator=(const VaSource&) = delete;

    char next_char() { return static_cast<char>(va_arg(args_, int)); }

    detail::TextView next_string() {
        const char* source = va_arg(args_, const char*);
        if (source == nullptr) {
            source = "(null)";
        }
        return {.data = source, .length = static_cast<int>(__builtin_strlen(source))};
    }

    int64_t next_signed(int length) {
        if (length == 0) {
            return va_arg(args_, int);
        }
        return va_arg(args_, long long);
    }

    uint64_t next_unsigned(int length) {
        if (length == 0) {
            return va_arg(args_, unsigned int);
        }
        return va_arg(args_, unsigned long long);
    }

    uintptr_t next_pointer() { return reinterpret_cast<uintptr_t>(va_arg(args_, void*)); }

private:
    va_list args_{};
};

void buffer_emit(char character, void* context) {
    auto* sink = static_cast<detail::BufferSink*>(context);
    if (sink->position + 1 < sink->capacity) {
        sink->data[sink->position] = character;
        ++sink->position;
    }
}

}  // namespace

void VformatToBuf(char* out_buffer, size_t buffer_size, const char* format, ...) {
    if (out_buffer == nullptr || buffer_size == 0) {
        return;
    }

    va_list args;
    va_start(args, format);

    detail::BufferSink buffer_sink{.data = out_buffer, .capacity = buffer_size, .position = 0};
    const detail::Sink kSink{.emit = &buffer_emit, .context = &buffer_sink};
    VaSource           source(args);
    detail::RunEngine(kSink, format, source);

    va_end(args);
    out_buffer[buffer_sink.position] = '\0';
}

}  // namespace cinux::base::format
