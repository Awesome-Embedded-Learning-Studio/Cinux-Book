#include "kernel/driver/net/e1000.hpp"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstdint>

#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/driver/net/e1000_config.hpp"
#include "kernel/driver/net/e1000_layout.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::driver::net {

namespace {

uint32_t reg_read(unsigned long regs, uint32_t offset) {
    return *base::PtrAt<volatile uint32_t>(regs + offset);
}

void reg_write(unsigned long regs, uint32_t offset, uint32_t value) {
    *base::PtrAt<volatile uint32_t>(regs + offset) = value;
}

}  // namespace

bool E1000::init() {
    if (present_) {
        return true;
    }
    const PciDevice* card = PciBus::self().find_by_ids(kE1000VendorId, kE1000DeviceId);
    if (card == nullptr) {
        return false;
    }
    PciBus::self().enable_device(*card);

    const uint64_t kWindow = BarBase(card->bars, 0);
    if (kWindow == 0 || !mm::MapDeviceWindow(kWindow, kE1000WindowBytes)) {
        return false;
    }
    regs_ = mm::IoremapVirt(static_cast<unsigned long>(kWindow));

    reg_write(regs_, e1000::kRegCtrl, e1000::kCtrlReset);
    for (unsigned long spin = 0; spin < 1000; ++spin) {
        if ((reg_read(regs_, e1000::kRegCtrl) & e1000::kCtrlReset) == 0) {
            break;
        }
    }
    reg_write(regs_, e1000::kRegImc, 0xFFFFFFFF);
    reg_write(regs_, e1000::kRegCtrl, reg_read(regs_, e1000::kRegCtrl) | e1000::kCtrlSetLinkUp);

    read_mac();
    uint32_t address_low  = 0;
    uint32_t address_high = 0;
    for (unsigned long index = 0; index < kMacAddressBytes - 2; ++index) {
        address_low |= static_cast<uint32_t>(mac_.bytes[index]) << (8U * index);
    }
    for (unsigned long index = 0; index < 2; ++index) {
        address_high |= static_cast<uint32_t>(mac_.bytes[4 + index]) << (8U * index);
    }
    reg_write(regs_, e1000::kRegRal, address_low);
    reg_write(regs_, e1000::kRegRah, address_high | e1000::kRahAddressValid);

    ring_page_ = mm::Pmm::self().allocate_page();
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(ring_page_.raw)), 0, arch::page::kSize);
    for (auto& buffer : rx_buffers_) {
        buffer = mm::Pmm::self().allocate_page();
    }
    for (auto& buffer : tx_buffers_) {
        buffer = mm::Pmm::self().allocate_page();
    }

    auto* const kRxRing = base::PtrAt<e1000::RxDescriptor>(mm::DirectMapVirt(ring_page_.raw));
    for (unsigned long index = 0; index < kE1000RxRingSize; ++index) {
        kRxRing[index].address = rx_buffers_[index].raw;
        kRxRing[index].status  = 0;
    }

    const uint64_t kRxBase = ring_page_.raw;
    const uint64_t kTxBase = ring_page_.raw + (kE1000RxRingSize * sizeof(e1000::RxDescriptor));
    reg_write(regs_, e1000::kRegRdbal, static_cast<uint32_t>(kRxBase));
    reg_write(regs_, e1000::kRegRdbah, static_cast<uint32_t>(kRxBase >> 32U));
    reg_write(regs_, e1000::kRegRdlen, kE1000RxRingSize * sizeof(e1000::RxDescriptor));
    rx_tail_ = 0;
    reg_write(regs_, e1000::kRegRdt, kE1000RxRingSize - 1);

    reg_write(regs_, e1000::kRegTdbal, static_cast<uint32_t>(kTxBase));
    reg_write(regs_, e1000::kRegTdbah, static_cast<uint32_t>(kTxBase >> 32U));
    reg_write(regs_, e1000::kRegTdlen, kE1000TxRingSize * sizeof(e1000::TxDescriptor));
    reg_write(regs_, e1000::kRegTdh, 0);
    tx_next_ = 0;
    reg_write(regs_, e1000::kRegTdt, 0);

    reg_write(regs_, e1000::kRegTctl, e1000::kTctlEnable | e1000::kTctlPadShortPackets);
    reg_write(regs_, e1000::kRegRctl,
              e1000::kRctlEnable | e1000::kRctlBroadcastAccept | e1000::kRctlStripCrc);

    seat_.adopt(*this);
    present_ = true;
    return true;
}

bool E1000::present() const {
    return present_;
}

unsigned long E1000::tx_head() const {
    return reg_read(regs_, e1000::kRegTdh);
}

unsigned long E1000::rx_head() const {
    return reg_read(regs_, e1000::kRegRdh);
}

NetPort& E1000::port() {
    return seat_;
}

bool E1000::has_mac() const {
    return present_;
}

MacAddress E1000::mac() const {
    return mac_;
}

bool E1000::has_ethernet_header() const {
    return true;
}

unsigned long E1000::max_frame() const {
    return kMaxEthernetFrame;
}

bool E1000::poll_frame(RxFrame& into) {
    const uint32_t kHead = reg_read(regs_, e1000::kRegRdh);
    if (kHead == rx_tail_) {
        return false;
    }
    auto* const kRing = base::PtrAt<e1000::RxDescriptor>(mm::DirectMapVirt(ring_page_.raw));
    if ((kRing[rx_tail_].status & e1000::kRxStatusDone) == 0) {
        return false;
    }
    const unsigned long kLength =
        std::min(static_cast<unsigned long>(kRing[rx_tail_].length), kMaxEthernetFrame);
    const auto* const kBytes =
        base::PtrAt<const unsigned char>(mm::DirectMapVirt(rx_buffers_[rx_tail_].raw));
    for (unsigned long index = 0; index < kLength; ++index) {
        into.bytes[index] = kBytes[index];
    }
    into.size = kLength;

    kRing[rx_tail_].status        = 0;
    kRing[rx_tail_].errors        = 0;
    rx_tail_                      = (rx_tail_ + 1) % kE1000RxRingSize;
    const unsigned long kSentinel = (rx_tail_ + kE1000RxRingSize - 1) % kE1000RxRingSize;
    reg_write(regs_, e1000::kRegRdt, static_cast<uint32_t>(kSentinel));
    return true;
}

bool E1000::send_l3(const MacAddress& destination, EtherType type, const std::uint8_t* payload,
                    unsigned long length) {
    if (!present_ || length > kE1000TxBufferBytes - kEthernetHeaderBytes) {
        return false;
    }
    auto* const kFrame = base::PtrAt<unsigned char>(mm::DirectMapVirt(tx_buffers_[tx_next_].raw));
    for (unsigned long index = 0; index < kMacAddressBytes; ++index) {
        kFrame[index]                    = destination.bytes[index];
        kFrame[kMacAddressBytes + index] = mac_.bytes[index];
    }
    base::WriteWord16Be(kFrame, kEthernetHeaderBytes - sizeof(uint16_t), type.value);
    for (unsigned long index = 0; index < length; ++index) {
        kFrame[kEthernetHeaderBytes + index] = payload[index];
    }

    auto* const kRing = base::PtrAt<e1000::TxDescriptor>(
        mm::DirectMapVirt(ring_page_.raw) + (kE1000RxRingSize * sizeof(e1000::RxDescriptor)));
    kRing[tx_next_].address = tx_buffers_[tx_next_].raw;
    kRing[tx_next_].length  = static_cast<uint16_t>(kEthernetHeaderBytes + length);
    kRing[tx_next_].command =
        e1000::kTxCmdEndOfPacket | e1000::kTxCmdInsertCrc | e1000::kTxCmdReportStatus;

    tx_next_ = (tx_next_ + 1) % kE1000TxRingSize;
    reg_write(regs_, e1000::kRegTdt, static_cast<uint32_t>(tx_next_));
    reg_write(regs_, e1000::kRegTctl, e1000::kTctlEnable | e1000::kTctlPadShortPackets);
    return true;
}

void E1000::read_mac() {
    uint16_t words[3] = {};
    for (uint32_t word = 0; word < 3; ++word) {
        reg_write(regs_, e1000::kRegEerd, e1000::kEerdStart | (word << e1000::kEerdAddressShift));
        for (unsigned long spin = 0; spin < 10000; ++spin) {
            const uint32_t kAnswer = reg_read(regs_, e1000::kRegEerd);
            if ((kAnswer & e1000::kEerdDone) != 0) {
                words[word] = static_cast<uint16_t>(kAnswer >> e1000::kEerdDataShift);
                break;
            }
        }
    }
    for (unsigned long index = 0; index < kMacAddressBytes; ++index) {
        const auto kWord  = static_cast<unsigned>(words[index / 2]);
        mac_.bytes[index] = static_cast<unsigned char>((kWord >> (8U * (index % 2))) & 0xFFU);
    }
}

}  // namespace cinux::driver::net
