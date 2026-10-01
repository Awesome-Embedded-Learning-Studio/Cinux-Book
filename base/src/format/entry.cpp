/**
 * @file    entry.cpp
 * @brief   Every-world entry point of the format engine.
 *
 * FormatToBuf takes a type-erased Arg array: plain struct passing keeps one
 * calling convention in every world, including -m16 boot, where C variadic
 * slots are unusable (callers and va_arg disagree on slot width there).
 * The host-only variadic sugar lives in entry_variadic.cpp.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_format
 */

#include <stddef.h>
#include <stdint.h>

#include <cinux/format.hpp>
#include <cinux/ptr.hpp>

#include "detail.hpp"

namespace cinux::base::format {

namespace {

void buffer_emit(char character, void* context) {
    auto* sink = static_cast<detail::BufferSink*>(context);
    if (sink->position + 1 < sink->capacity) {
        sink->data[sink->position] = character;
        ++sink->position;
    }
}

class ArraySource {
public:
    ArraySource(const Arg* args, size_t count) : args_(args), count_(count) {}

    char next_char() { return static_cast<char>(take().value); }

    detail::TextView next_string() {
        const char* source = base::PtrAt<char>(static_cast<unsigned long>(take().value));
        if (source == nullptr) {
            source = "(null)";
        }
        return {.data = source, .length = static_cast<int>(__builtin_strlen(source))};
    }

    int64_t next_signed([[maybe_unused]] int length) { return static_cast<int64_t>(take().value); }

    uint64_t next_unsigned([[maybe_unused]] int length) { return take().value; }

    uintptr_t next_pointer() { return static_cast<uintptr_t>(take().value); }

private:
    const Arg& take() {
        if (position_ >= count_) {
            return kExhausted;
        }
        return args_[position_++];
    }

    static constexpr Arg kExhausted{.kind = Arg::Kind::kUnsigned, .value = 0};

    const Arg* args_;
    size_t     count_;
    size_t     position_ = 0;
};

}  // namespace

void FormatToBuf(char* out_buffer, size_t buffer_size, const char* format, ArgsView args) {
    if (out_buffer == nullptr || buffer_size == 0) {
        return;
    }

    detail::BufferSink buffer_sink{.data = out_buffer, .capacity = buffer_size, .position = 0};
    const detail::Sink kSink{.emit = &buffer_emit, .context = &buffer_sink};
    ArraySource        source(args.data, args.count);
    detail::RunEngine(kSink, format, source);

    out_buffer[buffer_sink.position] = '\0';
}

}  // namespace cinux::base::format
