/**
 * @file kernel/drivers/apic/local_apic.cpp
 * @brief Local APIC (xAPIC MMIO) driver implementation (F4-M2)
 */

#include "local_apic.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::apic {

LocalAPIC g_lapic;

void LocalAPIC::bind(volatile uint32_t* mmio_base) {
    base_ = mmio_base;
}

bool LocalAPIC::init(uint64_t mmio_phys) {
    // MMIO window inside KMEM_MMIO, offset 64 KB past AHCI BAR5 (which owns
    // KMEM_MMIO_BASE).  FLAG_PCD: APIC registers are uncached MMIO, not RAM --
    // never access them through the cache-enabled direct map.
    constexpr uint64_t kLapicMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x10000;
    constexpr uint64_t kFlags =
        cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
    if (!cinux::mm::g_vmm.map(kLapicMmioVirt, mmio_phys, kFlags)) {
        return false;
    }
    bind(reinterpret_cast<volatile uint32_t*>(kLapicMmioVirt));
    return true;
}

uint32_t LocalAPIC::read(uint32_t off) const {
    return *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base_) + off);
}

void LocalAPIC::write(uint32_t off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base_) + off) = value;
}

uint32_t LocalAPIC::id() const {
    return read(kRegId) >> 24;
}
uint32_t LocalAPIC::version() const {
    return read(kRegVersion) & 0xFF;
}

void LocalAPIC::enable(uint8_t spurious_vector) {
    write(kRegSpurious, static_cast<uint32_t>(spurious_vector) | kSvrEnable);
}

void LocalAPIC::disable() {
    write(kRegSpurious, read(kRegSpurious) & ~kSvrEnable);
}

void LocalAPIC::eoi() {
    write(kRegEoi, 0);
}

uint32_t LocalAPIC::error_status() {
    return read(kRegErrorStatus);
}

void LocalAPIC::clear_error() {
    write(kRegErrorStatus, 0);
}

void LocalAPIC::send_ipi(uint8_t dest_apic_id, uint8_t vector) {
    write(kRegIcrHigh, static_cast<uint32_t>(dest_apic_id) << 24);
    while ((read(kRegIcrLow) & kIcrDeliveryStatus) != 0) {
        // Wait for any previous IPI to finish sending (delivery status Idle).
    }
    write(kRegIcrLow, static_cast<uint32_t>(vector) | kIcrModeFixed);
}

void LocalAPIC::send_init(uint8_t dest_apic_id) {
    write(kRegIcrHigh, static_cast<uint32_t>(dest_apic_id) << 24);
    while ((read(kRegIcrLow) & kIcrDeliveryStatus) != 0) {
    }
    write(kRegIcrLow, kIcrModeInit | kIcrLevelAssert);
}

void LocalAPIC::send_sipi(uint8_t dest_apic_id, uint8_t vector) {
    write(kRegIcrHigh, static_cast<uint32_t>(dest_apic_id) << 24);
    while ((read(kRegIcrLow) & kIcrDeliveryStatus) != 0) {
    }
    write(kRegIcrLow, static_cast<uint32_t>(vector) | kIcrModeSipi | kIcrLevelAssert);
}

}  // namespace cinux::drivers::apic
