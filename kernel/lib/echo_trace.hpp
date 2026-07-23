#pragma once

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"

namespace cinux::debug {

constexpr bool kEchoTrace = false;

inline void trace_char(const char* tag, char ch) {
    if (!kEchoTrace) {
        return;
    }
    if (ch == '\n') {
        cinux::lib::kprintf("[ECHO_TRACE] %s ch=\\n(0x0a)\n", tag);
    } else if (ch == '\r') {
        cinux::lib::kprintf("[ECHO_TRACE] %s ch=\\r(0x0d)\n", tag);
    } else if (ch == '\b') {
        cinux::lib::kprintf("[ECHO_TRACE] %s ch=\\b(0x08)\n", tag);
    } else if (static_cast<uint8_t>(ch) >= 0x20 && static_cast<uint8_t>(ch) <= 0x7e) {
        cinux::lib::kprintf("[ECHO_TRACE] %s ch='%c'(0x%x)\n", tag, ch,
                            static_cast<unsigned>(static_cast<uint8_t>(ch)));
    } else {
        cinux::lib::kprintf("[ECHO_TRACE] %s ch=0x%x\n", tag,
                            static_cast<unsigned>(static_cast<uint8_t>(ch)));
    }
}

inline void trace_bytes(const char* tag, const void* data, int64_t n) {
    if (!kEchoTrace || data == nullptr || n <= 0) {
        return;
    }
    const auto* buf = static_cast<const char*>(data);
    cinux::lib::kprintf("[ECHO_TRACE] %s n=%d data=\"", tag, static_cast<int>(n));
    int64_t limit = n < 48 ? n : 48;
    for (int64_t i = 0; i < limit; ++i) {
        char ch = buf[i];
        if (ch == '\n') {
            cinux::lib::kprintf("\\n");
        } else if (ch == '\r') {
            cinux::lib::kprintf("\\r");
        } else if (ch == '\b') {
            cinux::lib::kprintf("\\b");
        } else if (static_cast<uint8_t>(ch) >= 0x20 && static_cast<uint8_t>(ch) <= 0x7e) {
            cinux::lib::kprintf("%c", ch);
        } else {
            cinux::lib::kprintf("\\x%x", static_cast<unsigned>(static_cast<uint8_t>(ch)));
        }
    }
    if (n > limit) {
        cinux::lib::kprintf("...");
    }
    cinux::lib::kprintf("\"\n");
}

}  // namespace cinux::debug
