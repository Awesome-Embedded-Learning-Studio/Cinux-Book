#pragma once
#include <stdint.h>

namespace cinux::mini::io {

// Inline I/O functions (x86 in/out instructions)
inline uint8_t inb(uint16_t port) {
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

}  // namespace cinux::mini::io
