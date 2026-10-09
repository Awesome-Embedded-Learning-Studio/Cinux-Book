#include "kernel/driver/base/io.hpp"

#include <stdint.h>

namespace cinux::driver {

namespace {

void out_port(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t in_port(uint16_t port) {
    uint8_t value = 0;  // NOLINT(misc-const-correctness)
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void out_port32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t in_port32(uint16_t port) {
    uint32_t value = 0;  // NOLINT(misc-const-correctness)
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

}  // namespace

void OutB(PortWrite write) {
    out_port(write.port, write.value);
}

uint8_t InB(uint16_t port) {
    return in_port(port);
}

void Out32(PortWrite32 write) {
    out_port32(write.port, write.value);
}

uint32_t In32(uint16_t port) {
    return in_port32(port);
}

void WaitBitsSet(uint16_t port, uint8_t mask) {
    while ((in_port(port) & mask) == 0) {
        asm volatile("pause");
    }
}

void WaitBitsClear(uint16_t port, uint8_t mask) {
    while ((in_port(port) & mask) != 0) {
        asm volatile("pause");
    }
}

}  // namespace cinux::driver
