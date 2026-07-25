/**
 * @file kernel/drivers/pci/msix.cpp
 * @brief PCI MSI-X capability discovery implementation
 *
 * Implements find_capability(): capability-list walk + MSI-X Table/PBA
 * decode.  Pure -- no MMIO or DMA, reads config space only through the
 * injected ConfigReader, so the same object code runs in the kernel and in
 * host unit tests.
 *
 * Batch 0A scope: discovery only.  Batch 0B adds Table/PBA MMIO mapping and
 * entry programming; Batch 0C adds the IDT vector-install helper.
 */

#include "msix.hpp"

#include <stdint.h>

#include "pci_config.hpp"

namespace cinux::drivers::pci::msix {

namespace {
/// Hard cap on capability-list length to bound the walk against corrupt or
/// looping next-pointers (the real spec limit is far smaller).
constexpr uint8_t kMaxCapabilities = 48;
}  // namespace

MsixCap find_capability(uint8_t bus, uint8_t slot, uint8_t func, ConfigReader reader) {
    MsixCap cap{};

    // STATUS (offset 0x06) bit 4 indicates a capability list is present.
    // STATUS is the high word of the COMMAND/STATUS dword at offset 0x04.
    const uint32_t cmd_status = reader(bus, slot, func, PciReg::COMMAND);
    const uint16_t status     = static_cast<uint16_t>((cmd_status >> 16) & 0xFFFF);
    if ((status & PciStatus::CAP_LIST) == 0) {
        return cap;  // No capability list -> no MSI-X
    }

    // First capability offset is the low byte of the dword at 0x34.
    const uint32_t ptr_dword = reader(bus, slot, func, PciReg::CAPABILITIES_POINTER);
    uint8_t        offset    = static_cast<uint8_t>(ptr_dword & 0xFF);
    if (offset == 0) {
        return cap;  // Empty list
    }

    // Walk the singly-linked capability list.
    for (uint8_t i = 0; i < kMaxCapabilities && offset != 0; ++i) {
        // dword at offset: [cap_id(b0) | next_ptr(b1) | msg_ctrl_lo(b2) | msg_ctrl_hi(b3)]
        const uint32_t dword0 = reader(bus, slot, func, offset);
        const uint8_t  cap_id = static_cast<uint8_t>(dword0 & 0xFF);
        const uint8_t  next   = static_cast<uint8_t>((dword0 >> 8) & 0xFF);

        if (cap_id == PciCapId::MSI_X) {
            cap.found           = true;
            cap.cap_offset      = offset;
            cap.message_control = static_cast<uint16_t>((dword0 >> 16) & 0xFFFF);
            cap.table_size =
                static_cast<uint16_t>((cap.message_control & MsixMsgCtrl::kTableSizeMask) + 1);

            const uint32_t table_dword = reader(bus, slot, func, static_cast<uint8_t>(offset + 4));
            cap.table_offset           = table_dword & MsixBarOffset::kOffsetMask;
            cap.table_bar = static_cast<uint8_t>(table_dword & MsixBarOffset::kBirMask);

            const uint32_t pba_dword = reader(bus, slot, func, static_cast<uint8_t>(offset + 8));
            cap.pba_offset           = pba_dword & MsixBarOffset::kOffsetMask;
            cap.pba_bar              = static_cast<uint8_t>(pba_dword & MsixBarOffset::kBirMask);
            return cap;
        }
        offset = next;
    }

    return cap;  // MSI-X not present in the list
}

// ============================================================
// Pure helpers (host-testable): xAPIC message format + Message Control bits
// ============================================================

uint32_t xapic_message_address(uint8_t dest_apic_id) {
    // xAPIC MSI address: 0xFEE00000 base, destination APIC ID in bits [19:12],
    // physical destination mode, no redirection hint.
    return 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12);
}

uint32_t xapic_message_data(uint8_t vector) {
    // Fixed delivery mode, edge-triggered, physical destination: the vector
    // occupies the low 8 bits of Message Data.
    return static_cast<uint32_t>(vector);
}

uint16_t message_control_with_enable(uint16_t raw) {
    return static_cast<uint16_t>(raw | MsixMsgCtrl::kEnable);
}

uint16_t message_control_unmask_function(uint16_t raw) {
    return static_cast<uint16_t>(raw & ~MsixMsgCtrl::kFunctionMask);
}

}  // namespace cinux::drivers::pci::msix
