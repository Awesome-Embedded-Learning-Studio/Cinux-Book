/**
 * @file kernel/drivers/pci/msix.hpp
 * @brief PCI MSI-X capability discovery (pure, reader-injected)
 *
 * Walks a PCI function's capability list to locate the MSI-X capability
 * (id 0x11) and decodes its Table/PBA location and size.  Config-space
 * access is injected through a ConfigReader callback so the same code runs
 * in the kernel (real 0xCF8/0xCFC reads) and in host unit tests (mock
 * config-space image).
 *
 * Scope (Batch 0A): discovery only.  Table/PBA MMIO mapping + entry
 * programming (Batch 0B) and the IDT vector-install helper (Batch 0C) are
 * layered on top of this in msix.cpp.
 *
 * Namespace: cinux::drivers::pci::msix
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

namespace cinux::drivers::pci {
struct PCIDevice;  // forward declaration; full type lives in pci.hpp (.cpp only)
}

namespace cinux::drivers::pci::msix {

// ============================================================
// Config-space reader callback
// ============================================================

/// Signature matches PCI::pci_read (pci.hpp): returns the 32-bit dword at
/// the dword-aligned config offset of function bus/slot/func.  Injecting
/// this keeps find_capability() free of any real I/O.
using ConfigReader = uint32_t (*)(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

// ============================================================
// MSI-X Message Control bits (word at cap_offset + 2)
// ============================================================

namespace MsixMsgCtrl {
constexpr uint16_t kTableSizeMask = 0x07FF;    ///< Bits [10:0]: Table Size (entries = field + 1)
constexpr uint16_t kFunctionMask  = 1U << 14;  ///< Bit 14: Function Mask (global masking)
constexpr uint16_t kEnable        = 1U << 15;  ///< Bit 15: MSI-X Enable
}  // namespace MsixMsgCtrl

// ============================================================
// MSI-X Table / PBA BAR-offset decode (dword at cap_offset + 4 / + 8)
// ============================================================

namespace MsixBarOffset {
constexpr uint32_t kBirMask    = 0x00000007;  ///< Bits [2:0]: BAR Indicator Register (0-5)
constexpr uint32_t kOffsetMask = 0xFFFFFFF8;  ///< Bits [31:3]: offset within the BAR (8-aligned)
}  // namespace MsixBarOffset

// ============================================================
// Parsed MSI-X capability
// ============================================================

/**
 * @brief Decoded MSI-X capability for one PCI function
 *
 * Populated by find_capability().  When @p found is false every other field
 * is zero-initialised.
 */
struct MsixCap {
    bool     found;            ///< MSI-X capability present on this function
    uint8_t  cap_offset;       ///< Config-space offset of the MSI-X capability
    uint16_t message_control;  ///< Raw Message Control word
    uint16_t table_size;       ///< Number of Table entries = (MC[10:0] + 1)
    uint32_t table_offset;     ///< Byte offset of the Table within its BAR
    uint8_t  table_bar;        ///< BIR (0-5) of the BAR holding the Table
    uint32_t pba_offset;       ///< Byte offset of the PBA within its BAR
    uint8_t  pba_bar;          ///< BIR of the BAR holding the PBA
};

/**
 * @brief Walk the capability list of a PCI function and locate MSI-X
 *
 * Reads STATUS (0x06) to confirm a capability list exists, then follows the
 * singly-linked list from the pointer at 0x34 until the MSI-X capability
 * (id 0x11) is found or the list ends.  Performs no MMIO or DMA -- only
 * config reads through @p reader -- so it links and runs identically in the
 * kernel and in host unit tests.
 *
 * @param bus     PCI bus number
 * @param slot    PCI slot number
 * @param func    PCI function number
 * @param reader  Config-space dword reader (kernel: &PCI::pci_read; test: mock)
 * @return        Decoded MsixCap (.found=false if MSI-X is absent)
 */
MsixCap find_capability(uint8_t bus, uint8_t slot, uint8_t func, ConfigReader reader);

// ============================================================
// MSI-X Table entry (16 bytes, MMIO-resident inside a device BAR)
// ============================================================

/// MSI-X Table entry.  Members are volatile: both the CPU and controller
/// access it through the BAR MMIO window.
struct MsixTableEntry {
    volatile uint32_t msg_addr_lower;  ///< +0:  Message Address (lower 32 bits)
    volatile uint32_t msg_addr_upper;  ///< +4:  Message Address (upper 32 bits, 0 on xAPIC)
    volatile uint32_t msg_data;        ///< +8:  Message Data
    volatile uint32_t vector_control;  ///< +12: bit 0 = Mask (1 masks this vector)
};
static_assert(sizeof(MsixTableEntry) == 16, "MSI-X Table entry must be 16 bytes");

/// Vector Control bit 0: mask this entry's vector.
constexpr uint32_t kEntryMaskBit = 0x1;

// ============================================================
// Pure helpers (host-testable: xAPIC message format + Message Control bits)
// ============================================================

/// xAPIC (32-bit) MSI Message Address: 0xFEE00000 base, dest APIC ID in
/// [19:12], physical destination mode, no redirection hint.  x2APIC is not
/// supported by the kernel.
uint32_t xapic_message_address(uint8_t dest_apic_id);

/// xAPIC MSI Message Data: vector in [7:0], fixed delivery, edge-triggered.
uint32_t xapic_message_data(uint8_t vector);

/// Set the MSI-X Enable bit (Message Control bit 15).
uint16_t message_control_with_enable(uint16_t raw);

/// Clear the Function Mask bit (Message Control bit 14) so the per-entry
/// masks take effect.
uint16_t message_control_unmask_function(uint16_t raw);

// ============================================================
// MSI-X controller (kernel-only: Table/PBA MMIO map + entry programming)
// ============================================================

/**
 * @brief Owns one function's MSI-X Table + PBA MMIO and programs vectors
 *
 * init() maps the Table and PBA; mask_all() / program_vector() manipulate
 * Table entries; enable() flips Message Control Enable.  Kernel-only (uses
 * VMM::map + PCI config writes) -- NOT linked into host unit tests; the pure
 * helpers above are tested instead.  The end-to-end "vector fires" proof
 * lands in Batch 2C (xHCI doorbell -> event-ring interrupt).
 */
class MsixController {
public:
    /// Map the MSI-X Table + PBA for @p cap on @p dev.  Assumes
    /// cap.table_offset / cap.pba_offset are page-aligned (true for QEMU
    /// devices).  Does not enable MSI-X -- call enable() after programming.
    cinux::lib::ErrorOr<void> init(const MsixCap& cap, const PCIDevice& dev);

    /// Set the Mask bit on every Table entry.
    void mask_all();

    /// Program Table entry @p index with an xAPIC message for @p vector
    /// delivered to @p dest_apic_id, then unmask it.
    void program_vector(uint8_t index, uint8_t vector, uint8_t dest_apic_id);

    /// Enable MSI-X: set Message Control Enable (bit 15) and clear Function
    /// Mask (bit 14) via one config dword write that preserves the cap id +
    /// next-pointer in the low 16 bits.
    void enable();

    volatile MsixTableEntry* table() const { return table_; }

private:
    MsixCap                  cap_{};
    uint8_t                  bus_ = 0, slot_ = 0, func_ = 0;
    volatile MsixTableEntry* table_ = nullptr;
    volatile uint32_t*       pba_   = nullptr;
};

}  // namespace cinux::drivers::pci::msix
