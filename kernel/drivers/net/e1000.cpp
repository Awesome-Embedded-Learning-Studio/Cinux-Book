/**
 * @file kernel/drivers/net/e1000.cpp
 * @brief e1000 NIC: bring-up (批a) + legacy RX/TX rings + polled receive (批b)
 *
 * No interrupts yet.  Verified in QEMU (run-kernel-test with -device e1000):
 * find_e1000 finds the 82540em, init reads MAC=52:54:00:..., start_rx arms the
 * ring, and TX->SLIRP->RX round-trips prove reception -- an ARP request earns a
 * unicast ARP reply, and a DHCPDISCOVER earns a broadcast DHCPOFFER (both caught
 * by poll_rx).  Note: a polled RX MUST read an MMIO register (RDH) each poll so
 * the emulator delivers the packet -- see poll_rx.
 *
 * Namespace: cinux::drivers::net
 */

#include "e1000.hpp"

#include <stdint.h>

#include "e1000_mmio.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pci/pci_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::drivers::net {

E1000Controller* E1000Controller::s_instance_ = nullptr;

E1000Controller& E1000Controller::instance() {
    return *s_instance_;
}

void E1000Controller::set_instance(E1000Controller* c) {
    s_instance_ = c;
}

namespace {
// MMIO sub-allocation inside the KMEM_MMIO window.  In use: AHCI @+0x0, LAPIC
// @+0x10000, IOAPIC @+0x11000, xHCI BAR0 @+0x20000, MSI-X Table @+0x40000, PBA
// @+0x41000.  e1000 BAR0 (32-bit MMIO) slots at +0x50000 -- clear of the MSI-X
// block and far under the 2 MB ceiling.  8 pages (32 KB) cover every register
// 批a/批b touches (CTRL 0x0 .. MTA 0x5200 / RAL0 0x5400).
constexpr uint64_t kE1000MmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x50000;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
constexpr uint64_t kMmioPages = 8;
constexpr uint64_t kPageSize  = 4096;
constexpr uint32_t kPollIters = 100000;  // bounded poll cap (QEMU completes instantly)
}  // namespace

uint32_t E1000Controller::reg_read(uint32_t offset) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio_base_ + offset);
}

void E1000Controller::reg_write(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(mmio_base_ + offset) = value;
}

uint16_t E1000Controller::read_eeprom(uint8_t addr) {
    reg_write(e1000reg::EERD,
              (static_cast<uint32_t>(addr) << e1000reg::EERD_ADDR_SHIFT) | e1000reg::EERD_START);
    for (uint32_t i = 0; i < kPollIters; ++i) {
        if (reg_read(e1000reg::EERD) & e1000reg::EERD_DONE) {
            return static_cast<uint16_t>((reg_read(e1000reg::EERD) >> e1000reg::EERD_DATA_SHIFT) &
                                         0xFFFF);
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    return 0xFFFF;  // DONE never asserted
}

void E1000Controller::read_mac() {
    const uint16_t w0 = read_eeprom(0);
    const uint16_t w1 = read_eeprom(1);
    const uint16_t w2 = read_eeprom(2);
    mac_[0]           = static_cast<uint8_t>(w0 & 0xFF);
    mac_[1]           = static_cast<uint8_t>((w0 >> 8) & 0xFF);
    mac_[2]           = static_cast<uint8_t>(w1 & 0xFF);
    mac_[3]           = static_cast<uint8_t>((w1 >> 8) & 0xFF);
    mac_[4]           = static_cast<uint8_t>(w2 & 0xFF);
    mac_[5]           = static_cast<uint8_t>((w2 >> 8) & 0xFF);
}

cinux::lib::ErrorOr<void> E1000Controller::init(const pci::PCIDevice& dev) {
    dev_ = dev;

    // 1. Enable PCI Bus Master (DMA) + Memory Space (MMIO response).
    const uint32_t cmd = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND,
                        cmd | pci::PciCmd::BUS_MASTER | pci::PciCmd::MEM_SPACE);

    // 2. Map BAR0 (32-bit MMIO) into the MMIO window.
    const uint64_t bar0 = dev.bar[0];
    for (uint64_t i = 0; i < kMmioPages; ++i) {
        if (!cinux::mm::g_vmm.map(kE1000MmioVirt + i * kPageSize, bar0 + i * kPageSize,
                                  kMmioFlags)) {
            return cinux::lib::Error::OutOfMemory;
        }
    }
    mmio_base_ = kE1000MmioVirt;

    // 3. Software reset (CTRL.RST self-clears).  Bounded poll; non-fatal -- the
    //    EEPROM MAC read + link check are the 批a deliverables, and QEMU resets
    //    instantly.  Real HW completes in well under 1 ms.
    reg_write(e1000reg::CTRL, reg_read(e1000reg::CTRL) | e1000reg::CTRL_RST);
    bool rst_done = false;
    for (uint32_t i = 0; i < kPollIters; ++i) {
        if (!(reg_read(e1000reg::CTRL) & e1000reg::CTRL_RST)) {
            rst_done = true;
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    if (!rst_done) {
        cinux::lib::kprintf("[e1000] reset did not self-clear (CTRL=0x%x) -- continuing\n",
                            reg_read(e1000reg::CTRL));
    }

    // 4. Disable interrupts and force the link up.
    reg_write(e1000reg::IMC, 0xFFFFFFFF);
    reg_write(e1000reg::CTRL, reg_read(e1000reg::CTRL) | e1000reg::CTRL_SLU);

    // 5. Read the EEPROM MAC.
    read_mac();

    const uint32_t status = reg_read(e1000reg::STATUS);
    cinux::lib::kprintf("[e1000] BAR0=0x%lx MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%d FD=%d\n",
                        static_cast<unsigned long>(bar0), mac_[0], mac_[1], mac_[2], mac_[3],
                        mac_[4], mac_[5], static_cast<int>((status & e1000reg::STATUS_LU) != 0),
                        static_cast<int>((status & e1000reg::STATUS_FD) != 0));

    return {};
}

cinux::lib::ErrorOr<void> E1000Controller::start_rx() {
    // 1. RX descriptor ring (kRxDescCount * 16 B, page-aligned via DmaPool).
    auto d = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kRxDescCount) * 16);
    if (!d.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    desc_buf_  = std::move(d.value());
    auto* desc = static_cast<volatile RxDesc*>(desc_buf_.virt());
    for (uint32_t i = 0; i < kRxDescCount; ++i) {
        desc[i].buffer_addr = 0;
        desc[i].length      = 0;
        desc[i].checksum    = 0;
        desc[i].status      = 0;
        desc[i].errors      = 0;
        desc[i].special     = 0;
    }

    // 2. Packet buffers (one 2 KB page each); wire each into its descriptor.
    for (uint32_t i = 0; i < kRxDescCount; ++i) {
        auto b = cinux::drivers::dma::g_dma_pool.alloc(kRxBufSize);
        if (!b.ok()) {
            return cinux::lib::Error::OutOfMemory;  // earlier bufs freed by RAII
        }
        pkt_bufs_[i]        = std::move(b.value());
        desc[i].buffer_addr = pkt_bufs_[i].phys();
    }

    // 3. Program the unicast MAC (RAL0/RAH0, AV set) + clear the multicast
    //    table so the card accepts our frames (and, with UPE below, all frames).
    reg_write(e1000reg::RAL0,
              static_cast<uint32_t>(mac_[0]) | (static_cast<uint32_t>(mac_[1]) << 8) |
                  (static_cast<uint32_t>(mac_[2]) << 16) | (static_cast<uint32_t>(mac_[3]) << 24));
    reg_write(e1000reg::RAH0,
              static_cast<uint32_t>(mac_[4]) | (static_cast<uint32_t>(mac_[5]) << 8) | (1U << 31));
    for (uint32_t i = 0; i < 128; ++i) {  // MTA: 128 32-bit entries
        reg_write(e1000reg::MTA + i * 4, 0);
    }

    // 4. Program the ring registers.  RDT = N-1 leaves one descriptor as the
    //    sentinel so RDT never equals RDH (full/empty ambiguity).
    const uint64_t base = desc_buf_.phys();
    reg_write(e1000reg::RDBAL, static_cast<uint32_t>(base));
    reg_write(e1000reg::RDBAH, static_cast<uint32_t>(base >> 32));
    reg_write(e1000reg::RDLEN, kRxDescCount * 16);
    reg_write(e1000reg::RDH, 0);
    reg_write(e1000reg::RDT, kRxDescCount - 1);
    next_to_clean_ = 0;

    // 5. Enable the receiver: 2048 B buffers (BSIZE=00, BSEX=0), strip CRC,
    //    accept broadcast + unicast-promiscuous (loopback/self-test: any frame).
    reg_write(e1000reg::RCTL,
              e1000reg::RCTL_EN | e1000reg::RCTL_SECRC | e1000reg::RCTL_BAM | e1000reg::RCTL_UPE);

    cinux::lib::kprintf("[e1000] RX armed: %u descriptors, %u B buffers, RCTL=0x%x\n",
                        static_cast<unsigned>(kRxDescCount), static_cast<unsigned>(kRxBufSize),
                        reg_read(e1000reg::RCTL));
    return {};
}

cinux::lib::ErrorOr<void> E1000Controller::start_tx() {
    auto d = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kTxDescCount) * 16);
    if (!d.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    tx_desc_buf_ = std::move(d.value());
    auto* desc   = static_cast<volatile TxDesc*>(tx_desc_buf_.virt());
    for (uint32_t i = 0; i < kTxDescCount; ++i) {
        desc[i].buffer_addr = 0;
        desc[i].length      = 0;
        desc[i].cso         = 0;
        desc[i].cmd         = 0;
        desc[i].status      = 0;
        desc[i].css         = 0;
        desc[i].special     = 0;
    }

    auto b = cinux::drivers::dma::g_dma_pool.alloc(kTxBufSize);
    if (!b.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    tx_data_buf_ = std::move(b.value());

    const uint64_t base = tx_desc_buf_.phys();
    reg_write(e1000reg::TDBAL, static_cast<uint32_t>(base));
    reg_write(e1000reg::TDBAH, static_cast<uint32_t>(base >> 32));
    reg_write(e1000reg::TDLEN, kTxDescCount * 16);
    reg_write(e1000reg::TDH, 0);
    reg_write(e1000reg::TDT, 0);
    tx_tail_ = 0;

    reg_write(e1000reg::TCTL, e1000reg::TCTL_EN | e1000reg::TCTL_PSP);
    return {};
}

bool E1000Controller::poll_rx(uint8_t* dst, uint32_t max_len, uint32_t& out_len) {
    // Read RDH (MMIO).  On real HW the packet lands in the ring the instant it
    // arrives.  Under QEMU TCG, pulling the SLIRP reply into the ring needs the
    // device-model main loop to run, which a polled ring alone does not drive
    // -- the caller (test_e1000.cpp) sti+hlt's between polls so the main loop
    // runs and a LAPIC-timer IRQ wakes it (see main_test.cpp).  In production
    // the PIT/LAPIC tick already keeps the main loop alive, so this single read
    // is plenty.  Harmless on real HW.
    (void)reg_read(e1000reg::RDH);

    auto*            desc = static_cast<volatile RxDesc*>(desc_buf_.virt());
    volatile RxDesc& d    = desc[next_to_clean_];
    if ((d.status & kRxStatusDD) == 0) {
        return false;  // no packet at this slot
    }

    const uint32_t len = d.length;
    const uint32_t n   = (len < max_len) ? len : max_len;
    const uint8_t* src = static_cast<const uint8_t*>(pkt_bufs_[next_to_clean_].virt());
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = src[i];  // DMA-written buffer; byte copy (QEMU is DMA-coherent)
    }
    out_len = n;

    // Recycle the descriptor + advance the tail so HW can reuse the slot.
    d.status           = 0;
    d.buffer_addr      = pkt_bufs_[next_to_clean_].phys();
    next_to_clean_     = (next_to_clean_ + 1) % kRxDescCount;
    const uint32_t rdt = (next_to_clean_ == 0) ? (kRxDescCount - 1) : (next_to_clean_ - 1);
    reg_write(e1000reg::RDT, rdt);
    return true;
}

cinux::lib::ErrorOr<void> E1000Controller::send_packet(const uint8_t* data, uint32_t len) {
    if (len > kTxBufSize) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto* dst = static_cast<uint8_t*>(tx_data_buf_.virt());
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = data[i];
    }

    auto*          desc    = static_cast<volatile TxDesc*>(tx_desc_buf_.virt());
    const uint32_t slot    = tx_tail_;
    desc[slot].buffer_addr = tx_data_buf_.phys();
    desc[slot].length      = static_cast<uint16_t>(len);
    desc[slot].cso         = 0;
    desc[slot].cmd         = kTxCmdEOP | kTxCmdIFCS | kTxCmdRS;
    desc[slot].status      = 0;
    desc[slot].css         = 0;
    desc[slot].special     = 0;

    tx_tail_ = (tx_tail_ + 1) % kTxDescCount;
    reg_write(e1000reg::TDT, tx_tail_);  // ring doorbell -> HW DMAs + transmits

    // Bounded-poll for this descriptor's DD bit (transmit complete).
    for (uint32_t i = 0; i < kPollIters; ++i) {
        if (desc[slot].status & kTxStatusDD) {
            return {};
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    cinux::lib::kprintf("[e1000] TX did not complete (len=%u)\n", len);
    return cinux::lib::Error::TimedOut;
}

bool E1000Controller::link_up() const {
    if (mmio_base_ == 0) {
        return false;
    }
    return (reg_read(e1000reg::STATUS) & e1000reg::STATUS_LU) != 0;
}

void E1000Controller::mac(uint8_t out[6]) const {
    for (int i = 0; i < 6; ++i) {
        out[i] = mac_[i];
    }
}

void E1000Controller::rx_dump(const char* tag) const {
    // Diagnostic dump of the receiver state.  GPRC > 0 means the MAC has
    // accepted a packet (address filter passed) -- distinguishes a filter
    // rejection from a delivery/DMA problem.
    const auto* desc = static_cast<volatile RxDesc*>(desc_buf_.virt());
    cinux::lib::kprintf(
        "[e1000] %s: RCTL=0x%x RDH=%u RDT=%u GPRC=%u | d0addr=0x%lx d0st=0x%x d0len=%u\n", tag,
        reg_read(e1000reg::RCTL), reg_read(e1000reg::RDH), reg_read(e1000reg::RDT),
        reg_read(e1000reg::GPRC), static_cast<unsigned long>(desc[0].buffer_addr), desc[0].status,
        desc[0].length);
}

}  // namespace cinux::drivers::net
