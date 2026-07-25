/**
 * @file kernel/drivers/apic/io_apic.cpp
 * @brief I/O APIC driver implementation (F4-M2)
 */

#include "io_apic.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::apic {

IOAPIC g_ioapic;

void IOAPIC::bind(volatile uint32_t* mmio_base) {
    base_ = mmio_base;
}

bool IOAPIC::init(uint64_t mmio_phys) {
    // 4 KB past the Local APIC window (which is at KMEM_MMIO_BASE + 0x10000).
    constexpr uint64_t kIoapicMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x11000;
    constexpr uint64_t kFlags =
        cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
    if (!cinux::mm::g_vmm.map(kIoapicMmioVirt, mmio_phys, kFlags)) {
        return false;
    }
    bind(reinterpret_cast<volatile uint32_t*>(kIoapicMmioVirt));
    return true;
}

uint32_t IOAPIC::read(uint32_t reg) {
    base_[kIoapicIORegSel / 4] = reg;
    return base_[kIoapicIOWin / 4];
}

void IOAPIC::write(uint32_t reg, uint32_t value) {
    base_[kIoapicIORegSel / 4] = reg;
    base_[kIoapicIOWin / 4]    = value;
}

void IOAPIC::set_redirect(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, bool active_low,
                          bool level_triggered) {
    const uint32_t reg = kIoapicRegRedirectBase + gsi * 2;
    uint32_t       low = static_cast<uint32_t>(vector);
    if (active_low) {
        low |= kRedirectPolarityLow;
    }
    if (level_triggered) {
        low |= kRedirectTriggerLevel;
    }
    // Destination mode = physical (bit 11 = 0); destination APIC ID in high word bits 24-31.
    const uint32_t high = static_cast<uint32_t>(dest_apic_id) << 24;
    write(reg, low);
    write(reg + 1, high);
}

void IOAPIC::mask(uint32_t gsi) {
    const uint32_t reg = kIoapicRegRedirectBase + gsi * 2;
    write(reg, read(reg) | kRedirectMaskBit);
}

void IOAPIC::unmask(uint32_t gsi) {
    const uint32_t reg = kIoapicRegRedirectBase + gsi * 2;
    write(reg, read(reg) & ~kRedirectMaskBit);
}

uint32_t IOAPIC::id() {
    return (read(kIoapicRegId) >> 24) & 0x0F;
}

uint32_t IOAPIC::version() {
    return read(kIoapicRegVer) & 0xFF;
}

uint32_t IOAPIC::max_redirect() {
    return (read(kIoapicRegVer) >> 16) + 1;
}

}  // namespace cinux::drivers::apic
