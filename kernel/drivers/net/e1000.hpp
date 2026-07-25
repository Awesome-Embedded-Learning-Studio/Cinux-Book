/**
 * @file kernel/drivers/net/e1000.hpp
 * @brief Intel e1000 (8254x) NIC driver -- bring-up + legacy RX/TX (polled)
 *
 * 批a: PCI discover + reset + EEPROM MAC + link.  批b: legacy RX/TX descriptor
 * rings + polled receive/transmit.  RX reception is proven with real QEMU
 * user-net traffic -- an ARP request and a DHCPDISCOVER each make SLIRP answer,
 * and the unicast ARP reply + broadcast DHCPOFFER are both caught (QEMU's e1000
 * does NOT emulate RCTL.LBM internal loopback, so a self-loopback test is
 * useless here).  No interrupts yet (QEMU under nested-KVM does not reliably
 * latch MSI); the deferred batch adds MSI / legacy-INTx.
 *
 * Modelled on XHCIController (instance + singleton, init(const PCIDevice&)).
 * The e1000 register block is sparse, so access is reg_read / reg_write at the
 * named offsets in e1000_mmio.hpp.
 *
 * Namespace: cinux::drivers::net
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/pci/pci.hpp"

namespace cinux::drivers::net {

/// Legacy RX descriptor (16 bytes).  HW DMAs the packet into @p buffer_addr and
/// sets @p status.DD; EOP marks the last descriptor of a multi-descriptor packet
/// (a 2 KB buffer holds any standard-frame packet in one descriptor).
struct RxDesc {
    uint64_t buffer_addr;  ///< 64-bit address of the receive buffer
    uint16_t length;       ///< bytes DMA'd into the buffer
    uint16_t checksum;     ///< packet checksum (HW-filled)
    uint8_t  status;       ///< bit0 = DD (done), bit1 = EOP (end of packet)
    uint8_t  errors;       ///< descriptor error bits
    uint16_t special;      ///< reserved in legacy mode
};
static_assert(sizeof(RxDesc) == 16, "e1000 legacy RX descriptor must be 16 bytes");

constexpr uint8_t kRxStatusDD  = 1U << 0;  ///< RX Descriptor Done (HW filled the buffer)
constexpr uint8_t kRxStatusEOP = 1U << 1;  ///< RX End of Packet

/// Legacy TX descriptor (16 bytes).  SW fills @p buffer_addr + @p length + @p cmd
/// and advances TDT; HW DMAs the data, transmits, and sets @p status.DD (when
/// cmd.RS is set).
struct TxDesc {
    uint64_t buffer_addr;  ///< 64-bit address of the data buffer
    uint16_t length;       ///< data length in bytes
    uint8_t  cso;          ///< checksum offset (0 = none)
    uint8_t  cmd;          ///< command bits (EOP / IFCS / RS / ...)
    uint8_t  status;       ///< bit0 = DD (HW sets when done, if cmd.RS)
    uint8_t  css;          ///< checksum start
    uint16_t special;      ///< VLAN, etc (0 in legacy mode)
};
static_assert(sizeof(TxDesc) == 16, "e1000 legacy TX descriptor must be 16 bytes");

constexpr uint8_t kTxCmdEOP   = 1U << 0;  ///< TX End of Packet
constexpr uint8_t kTxCmdIFCS  = 1U << 1;  ///< TX Insert Frame Check Sequence
constexpr uint8_t kTxCmdRS    = 1U << 3;  ///< TX Report Status (set DD on done)
constexpr uint8_t kTxStatusDD = 1U << 0;  ///< TX Descriptor Done

class E1000Controller {
public:
    static E1000Controller& instance();
    static void             set_instance(E1000Controller* c);
    /// Null-safe probe: true once init() installed the singleton.
    static bool             has_controller() { return s_instance_ != nullptr; }

    /// Bring up the NIC from a PCI descriptor (PCI enable + BAR0 map + reset +
    /// EEPROM MAC + link).  批a.
    cinux::lib::ErrorOr<void> init(const pci::PCIDevice& dev);

    /// Allocate the RX descriptor ring + packet buffers, program RAL0/RAH0/MTA
    /// + RDBAL/RDBAH/RDLEN/RDH/RDT + RCTL, and arm the receiver.  批b.
    cinux::lib::ErrorOr<void> start_rx();

    /// Allocate the TX descriptor ring + one data buffer, program
    /// TDBAL/TDBAH/TDLEN/TDH/TDT + TCTL, and arm the transmitter.  批b.
    cinux::lib::ErrorOr<void> start_tx();

    /// Poll one received packet.  If the next descriptor's DD bit is set, copy
    /// up to @p max_len bytes into @p dst, recycle the descriptor, advance RDT,
    /// and return true (setting @p out_len).  False when nothing is pending.
    bool poll_rx(uint8_t* dst, uint32_t max_len, uint32_t& out_len);

    /// Copy @p len bytes into the TX data buffer, enqueue one descriptor
    /// (EOP|IFCS|RS), ring TDT, and bounded-poll until the descriptor's DD bit
    /// confirms the transmit completed.  批b.
    cinux::lib::ErrorOr<void> send_packet(const uint8_t* data, uint32_t len);

    /// STATUS.LU (informational; under QEMU the link is up once SLU is set).
    bool link_up() const;

    /// True once BAR0 is mapped (init() succeeded).
    bool present() const { return mmio_base_ != 0; }

    /// Copy the EEPROM MAC (6 octets) into @p out.
    void mac(uint8_t out[6]) const;

    /// Debug: dump RX register + first-descriptor state, tagged @p tag.
    void rx_dump(const char* tag) const;

private:
    uint32_t reg_read(uint32_t offset) const;
    void     reg_write(uint32_t offset, uint32_t value);
    uint16_t read_eeprom(uint8_t addr);
    void     read_mac();

    uint64_t       mmio_base_ = 0;  ///< BAR0 kernel-virtual base (0 = unmapped)
    uint8_t        mac_[6]    = {};
    pci::PCIDevice dev_{};  ///< retained for later MSI / capability use

    // RX ring state (批b).  DmaBuffer is move-only; the array holds empty
    // buffers until start_rx() allocates each one.
    static constexpr uint32_t      kRxDescCount = 32;    ///< RX descriptors
    static constexpr uint32_t      kRxBufSize   = 2048;  ///< bytes per RX buffer
    cinux::drivers::dma::DmaBuffer desc_buf_;
    cinux::drivers::dma::DmaBuffer pkt_bufs_[kRxDescCount];
    uint32_t                       next_to_clean_ = 0;

    // TX ring state (批b).  One reused data buffer; each send_packet() waits
    // for the descriptor's DD bit before returning, so at most one TX is in
    // flight.
    static constexpr uint32_t      kTxDescCount = 8;     ///< TX descriptors
    static constexpr uint32_t      kTxBufSize   = 2048;  ///< bytes per TX buffer
    cinux::drivers::dma::DmaBuffer tx_desc_buf_;
    cinux::drivers::dma::DmaBuffer tx_data_buf_;
    uint32_t                       tx_tail_ = 0;

    static E1000Controller* s_instance_;
};

}  // namespace cinux::drivers::net
