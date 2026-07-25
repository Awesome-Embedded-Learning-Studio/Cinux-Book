/**
 * @file kernel/drivers/pci/msix_controller.cpp
 * @brief MSI-X controller (kernel-only): Table/PBA MMIO mapping + programming
 *
 * MsixController::init maps the MSI-X Table and Pending Bit Array into the
 * KMEM_MMIO window (FLAG_PCD, uncached); mask_all / program_vector manipulate
 * Table entries; enable() flips Message Control Enable.  Kernel-only -- it
 * uses VMM::map and PCI config writes, so it is NOT linked into host unit
 * tests (the pure helpers in msix.cpp are tested instead).
 *
 * Batch 0B scope.  No caller yet -- the end-to-end "vector fires" proof lands
 * in Batch 2C (xHCI doorbell -> event-ring interrupt).
 *
 * Namespace: cinux::drivers::pci::msix
 */

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/vmm.hpp"
#include "msix.hpp"
#include "pci.hpp"

namespace cinux::drivers::pci::msix {

namespace {
// Fixed KMEM_MMIO sub-allocations.  These MUST NOT collide with the xHCI BAR0
// 4-page mapping (KMEM_MMIO+0x20000..+0x23FFF), which covers the xHCI runtime
// registers (@BAR0+RTSOFF) and doorbells (@BAR0+DBOFF).  Earlier slots are
// taken (AHCI @+0x0, LAPIC @+0x10000, IOAPIC @+0x11000, xHCI BAR0 @+0x20000).
constexpr uint64_t kMsixTableVirt = cinux::arch::KMEM_MMIO_BASE + 0x40000;
constexpr uint64_t kMsixPbaVirt   = cinux::arch::KMEM_MMIO_BASE + 0x41000;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
constexpr uint64_t kPageSize = 4096;

uint64_t bytes_to_pages(uint64_t bytes) {
    return (bytes + kPageSize - 1) / kPageSize;
}
}  // namespace

cinux::lib::ErrorOr<void> MsixController::init(const MsixCap& cap, const PCIDevice& dev,
                                               uint64_t table_virt, uint64_t pba_virt) {
    if (!cap.found) {
        return cinux::lib::Error::InvalidArgument;
    }
    cap_  = cap;
    bus_  = dev.bus;
    slot_ = dev.slot;
    func_ = dev.func;

    // Resolve the KMEM_MMIO sub-allocation.  The caller may override the default
    // (+0x40000 Table / +0x41000 PBA, used by xHCI) so a second controller can
    // map its Table/PBA elsewhere (NVMe uses +0x74000 / +0x75000).  0 = default.
    const uint64_t table_v = (table_virt != 0) ? table_virt : kMsixTableVirt;
    const uint64_t pba_v   = (pba_virt != 0) ? pba_virt : kMsixPbaVirt;

    // Map the MSI-X Table (cap.table_size entries x 16 bytes).  table_offset
    // is assumed page-aligned (true for QEMU); dev.bar[] is uint32_t so BARs
    // must sit below 4 GB (also true for QEMU).
    const uint64_t table_phys = static_cast<uint64_t>(dev.bar[cap.table_bar]) + cap.table_offset;
    const uint64_t table_pages =
        bytes_to_pages(static_cast<uint64_t>(cap.table_size) * sizeof(MsixTableEntry));
    for (uint64_t i = 0; i < table_pages; ++i) {
        if (!cinux::mm::g_vmm.map(table_v + i * kPageSize, table_phys + i * kPageSize,
                                  kMmioFlags)) {
            // Roll back the table pages already mapped before failing.
            for (uint64_t j = 0; j < i; ++j) {
                cinux::mm::g_vmm.unmap(table_v + j * kPageSize);
            }
            return cinux::lib::Error::OutOfMemory;
        }
    }
    table_ = reinterpret_cast<volatile MsixTableEntry*>(table_v);

    // Map the Pending Bit Array (1 bit per entry).
    const uint64_t pba_phys  = static_cast<uint64_t>(dev.bar[cap.pba_bar]) + cap.pba_offset;
    const uint64_t pba_pages = bytes_to_pages((static_cast<uint64_t>(cap.table_size) + 7) / 8);
    for (uint64_t i = 0; i < pba_pages; ++i) {
        if (!cinux::mm::g_vmm.map(pba_v + i * kPageSize, pba_phys + i * kPageSize, kMmioFlags)) {
            // Roll back: the PBA pages mapped so far, plus the whole table region.
            for (uint64_t j = 0; j < i; ++j) {
                cinux::mm::g_vmm.unmap(pba_v + j * kPageSize);
            }
            for (uint64_t j = 0; j < table_pages; ++j) {
                cinux::mm::g_vmm.unmap(table_v + j * kPageSize);
            }
            table_ = nullptr;  // table region was rolled back above
            return cinux::lib::Error::OutOfMemory;
        }
    }
    pba_ = reinterpret_cast<volatile uint32_t*>(pba_v);

    return {};
}

void MsixController::mask_all() {
    if (table_ == nullptr) {
        return;
    }
    for (uint16_t i = 0; i < cap_.table_size; ++i) {
        table_[i].vector_control = kEntryMaskBit;
    }
}

void MsixController::program_vector(uint8_t index, uint8_t vector, uint8_t dest_apic_id) {
    if (table_ == nullptr || index >= cap_.table_size) {
        return;
    }
    volatile MsixTableEntry& e = table_[index];
    e.vector_control           = kEntryMaskBit;  // mask while programming
    e.msg_addr_upper           = 0;
    e.msg_addr_lower           = xapic_message_address(dest_apic_id);
    e.msg_data                 = xapic_message_data(vector);
    static_cast<void>(e.vector_control);  // read-back orders the writes
    e.vector_control = 0;                 // unmask
}

void MsixController::enable() {
    // Message Control is the high word of the capability dword at cap_offset.
    // Rewrite the whole dword to preserve the capability id + next pointer.
    uint32_t dword0 = PCI::pci_read(bus_, slot_, func_, cap_.cap_offset);
    uint16_t mc     = static_cast<uint16_t>((dword0 >> 16) & 0xFFFF);
    mc              = message_control_unmask_function(message_control_with_enable(mc));
    dword0          = (dword0 & 0x0000FFFFu) | (static_cast<uint32_t>(mc) << 16);
    PCI::pci_write(bus_, slot_, func_, cap_.cap_offset, dword0);
}

}  // namespace cinux::drivers::pci::msix
