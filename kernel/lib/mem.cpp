#include "cinux/memory.hpp"
#include "kernel/arch/x86_64/halt.hpp"

// NOLINTBEGIN(readability-identifier-naming,misc-include-cleaner,readability-identifier-length)
// libc-spelled symbols the compiler may call
extern "C" {

void* memset(void* destination, int value, unsigned long bytes) {
    cinux::base::SetBytes(destination, static_cast<unsigned char>(value), bytes);
    return destination;
}

void* memcpy(void* destination, const void* source, unsigned long bytes) {
    cinux::base::CopyBytes(destination, source, bytes);
    return destination;
}

void* memmove(void* destination, const void* source, unsigned long bytes) {
    auto*       out = static_cast<unsigned char*>(destination);
    const auto* in  = static_cast<const unsigned char*>(source);
    if (out < in || out >= in + bytes) {
        cinux::base::CopyBytes(destination, source, bytes);
        return destination;
    }
    for (unsigned long index = bytes; index > 0; --index) {
        out[index - 1] = in[index - 1];
    }
    return destination;
}

const void* memchr(const void* area, int value, unsigned long bytes) {
    auto const* scan = static_cast<const unsigned char*>(area);
    for (unsigned long index = 0; index < bytes; ++index) {
        if (scan[index] == static_cast<unsigned char>(value)) {
            return &scan[index];
        }
    }
    return nullptr;
}

int memcmp(const void* left, const void* right, unsigned long bytes) {
    auto const* one = static_cast<const unsigned char*>(left);
    auto const* two = static_cast<const unsigned char*>(right);
    for (unsigned long index = 0; index < bytes; ++index) {
        if (one[index] != two[index]) {
            return one[index] < two[index] ? -1 : 1;
        }
    }
    return 0;
}

}  // extern "C"

namespace std {
// The -fno-exceptions landing for any throw the headers spell. A freestanding
// world provides the runtime symbols the headers call; this is that provision.
// NOLINTNEXTLINE(misc-use-internal-linkage,bugprone-std-namespace-modification)
[[noreturn]] void terminate() {
    cinux::arch::Halt();
}
}  // namespace std

// NOLINTEND(readability-identifier-naming,misc-include-cleaner,readability-identifier-length)
