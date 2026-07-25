/**
 * @file kernel/drivers/acpi/hpet.cpp
 * @brief HPET description-table parser (F5-M4)
 *
 * Decodes the ACPI "HPET" table into the one field the F5-M4 MMIO driver needs:
 * the physical base address of the HPET register block (a 4 KB window at
 * 0xFED00000 on QEMU).  The counter period and the free-running counter value
 * live in MMIO registers, not in this table, so they are read by the driver
 * after it maps the window -- this parser only extracts the base address.
 *
 * Parallel in structure to madt.cpp's parse_madt (validate, then reinterpret
 * through the [[gnu::packed]] table struct).
 */

#include <stddef.h>
#include <stdint.h>

#include "acpi.hpp"

namespace cinux::drivers::acpi {

/// ACPI Generic Address Structure address-space ID: system memory (MMIO).
constexpr uint8_t kAddrSpaceSystemMemory = 0;

HPETInfo parse_hpet(const SDTHeader* hpet) {
    HPETInfo info{};
    if (hpet == nullptr || hpet->length < sizeof(HPETHeader)) {
        return info;
    }

    const auto* h = reinterpret_cast<const HPETHeader*>(hpet);
    // The HPET register block is memory-mapped; reject anything the firmware
    // claims is in a different address space (system I/O, PCI, ...).
    if (h->base_addr_space != kAddrSpaceSystemMemory || h->base_address == 0) {
        return info;
    }

    info.address = h->base_address;
    info.present = true;
    return info;
}

}  // namespace cinux::drivers::acpi
