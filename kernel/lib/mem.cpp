#include "cinux/memory.hpp"

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

}  // extern "C"

// NOLINTEND(readability-identifier-naming,misc-include-cleaner,readability-identifier-length)
