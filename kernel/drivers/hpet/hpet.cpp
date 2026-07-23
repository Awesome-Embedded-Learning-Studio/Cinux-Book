/**
 * @file kernel/drivers/hpet/hpet.cpp
 * @brief HPET free-running counter driver implementation (F5-M4)
 *
 * Brings the HPET up from its ACPI description: find_table("HPET") -> parse_hpet
 * gives the physical MMIO base; we map a 4 KB window for it inside KMEM_MMIO
 * (FLAG_PCD, uncached -- the same treatment LAPIC/xHCI/e1000 MMIO get), read the
 * counter clock period from General Capabilities, set ENABLE_CNF so the main
 * counter actually increments, and snapshot the counter as the boot baseline.
 */

#include "hpet.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/acpi/acpi.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::drivers::acpi::HPETInfo;
using cinux::drivers::acpi::SDTHeader;
using cinux::drivers::acpi::find_table;
using cinux::drivers::acpi::parse_hpet;
using cinux::lib::kprintf;

namespace cinux::drivers {

// HPET MMIO sub-allocation inside the KMEM_MMIO window.  In use: AHCI @+0x0,
// LAPIC @+0x10000, IOAPIC @+0x11000, xHCI @+0x20000, MSI-X @+0x40000,
// e1000 @+0x50000 -- +0x60000 is free and leaves the existing windows alone.
// The HPET register block is 1 KB (well within a 4 KB page).
constexpr uint64_t kHpetMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x60000;
constexpr uint64_t kHpetMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;

HPET g_hpet;

uint32_t HPET::read32(uint32_t off) const {
    return *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base_) + off);
}

void HPET::write32(uint32_t off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(base_) + off) = value;
}

uint64_t HPET::read_counter() const {
    // Read the 64-bit counter as two 32-bit halves, guarding against the low
    // word rolling over between reads: if the high half changed, the low read
    // straddled a 4 GB boundary, so re-read low against the new high.
    uint32_t high1 = read32(kHpetRegMainCounter + 4);
    uint32_t low   = read32(kHpetRegMainCounter);
    uint32_t high2 = read32(kHpetRegMainCounter + 4);
    if (high1 != high2) {
        low = read32(kHpetRegMainCounter);
    }
    return (static_cast<uint64_t>(high2) << 32) | low;
}

bool HPET::init() {
    if (base_ != nullptr) {
        return true;  // idempotent
    }

    const SDTHeader* table = find_table("HPET");
    if (table == nullptr) {
        kprintf("[HPET] no ACPI HPET table; monotonic time stays PIT-backed\n");
        return false;
    }

    const HPETInfo info = parse_hpet(table);
    if (!info.present) {
        kprintf("[HPET] ACPI HPET table present but unparseable\n");
        return false;
    }

    if (!cinux::mm::g_vmm.map(kHpetMmioVirt, info.address, kHpetMmioFlags)) {
        kprintf("[HPET] failed to map MMIO window at phys 0x%lX\n",
                static_cast<unsigned long>(info.address));
        return false;
    }
    base_ = reinterpret_cast<volatile uint32_t*>(kHpetMmioVirt);

    // COUNTER_CLK_PERIOD occupies the HIGH 32 bits of General Capabilities
    // ([63:32]); the low 32 bits carry vendor ID / revision / timer-count caps.
    period_fs_ = read32(kHpetRegGeneralCaps + 4);
    if (period_fs_ == 0 || period_fs_ > kHpetMaxPeriodFs) {
        kprintf("[HPET] bogus counter period %lu fs; disabling\n",
                static_cast<unsigned long>(period_fs_));
        base_      = nullptr;
        period_fs_ = 0;
        return false;
    }

    // The main counter only increments once ENABLE_CNF (General Config bit 0) is
    // set; QEMU follows the spec, so without this the counter reads frozen.
    // Read-modify-write (32-bit) to preserve any firmware-set bits.
    write32(kHpetRegGeneralConfig, read32(kHpetRegGeneralConfig) | kHpetEnableCnf);

    // Snapshot the counter as the boot baseline so monotonic_ns is boot-relative.
    boot_counter_ = read_counter();

    const uint64_t freq_hz = 1'000'000'000'000'000ULL / period_fs_;
    kprintf("[HPET] MMIO 0x%lX, period %lu fs (%lu Hz), counter enabled\n",
            static_cast<unsigned long>(info.address), static_cast<unsigned long>(period_fs_),
            static_cast<unsigned long>(freq_hz));
    return true;
}

uint64_t HPET::counter() const {
    if (base_ == nullptr) {
        return 0;
    }
    return read_counter();
}

uint64_t HPET::monotonic_ns() const {
    if (base_ == nullptr) {
        return 0;
    }
    const uint64_t now = read_counter();
    return ticks_to_ns(period_fs_, now - boot_counter_);
}

}  // namespace cinux::drivers
