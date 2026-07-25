/**
 * @file kernel/drivers/mmio.hpp
 * @brief Memory-mapped I/O (MMIO) 32-bit access helpers.
 *
 * Centralises the volatile-uint32 deref over a byte-offset MMIO base used by
 * LocalAPIC, HPET (and other MMIO devices).  Byte-offset addressing keeps each
 * hardware register's documented offset intact (no element-vs-byte confusion).
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

inline uint32_t mmio32_read(volatile uint32_t* base, uint32_t off) {
    return *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base) + off);
}

inline void mmio32_write(volatile uint32_t* base, uint32_t off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base) + off) = value;
}

}  // namespace cinux::drivers
