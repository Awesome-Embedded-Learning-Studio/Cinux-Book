/**
 * @file kernel/drivers/net/e1000_net_device.hpp
 * @brief E1000NetDevice -- the adapter joining the e1000 driver to the L3 stack.
 *
 * The ONLY file that #includes BOTH e1000.hpp AND kernel/net/net_device.hpp.
 * Constructor-injected E1000Controller& (NEVER E1000Controller::instance() -- a
 * 2nd NIC's adapter would otherwise collide on the same ring; grep-enforced).
 *
 * COPY-based RX (the L0 design choice for e1000): poll_rx copies the DMA buffer
 * into an adapter-internal scratch and sets Packet.sink == nullptr (recycle is a
 * no-op -- the scratch is reused on the next poll).  send_l3 composes a 14-byte
 * EthHdr + the L3 payload into a local TX buffer; e1000::send_packet copies that
 * into its own TX DMA ring, so the local buffer need NOT be DMA-able.  RX
 * scratch and TX buffer are SEPARATE (a handler sending a reply during dispatch
 * does not clobber the in-flight RX frame).
 *
 * Namespace: cinux::drivers::net
 */

#pragma once

#include <cstdint>

#include "kernel/drivers/net/e1000.hpp"
#include "kernel/net/net_device.hpp"

namespace cinux::drivers::net {

class E1000NetDevice : public cinux::net::NetDevice {
public:
    explicit E1000NetDevice(E1000Controller& c) : ctrl_(c) {}

    bool mac(cinux::net::EthAddr& out) const override {
        ctrl_.mac(out.oct);
        return true;  // e1000 has a real EEPROM MAC
    }
    // has_ethernet_header() default true; max_frame() default 1518;
    // supports_zerocopy() default false (copy-based -- see file header).
    bool link_up() const override { return ctrl_.link_up(); }

    bool poll_rx(cinux::net::Packet& out) override {
        uint32_t len = 0;
        if (!ctrl_.poll_rx(rx_scratch_, static_cast<uint32_t>(sizeof(rx_scratch_)), len)) {
            return false;
        }
        out.data = rx_scratch_;
        out.len  = len;
        out.sink = nullptr;  // copy device: no BufferSink recycle
        return true;
    }

    cinux::lib::ErrorOr<void> send_l3(const cinux::net::EthAddr& next_hop, uint16_t ethertype,
                                      const uint8_t* l3, uint32_t len) override {
        if (len + 14 > sizeof(tx_buf_)) {
            return cinux::lib::Error::InvalidArgument;
        }
        // dst MAC = next_hop (ARP-resolved); src MAC = our EEPROM MAC.
        cinux::net::EthAddr src{};
        ctrl_.mac(src.oct);
        for (int i = 0; i < 6; ++i) {
            tx_buf_[i]     = next_hop.oct[i];
            tx_buf_[6 + i] = src.oct[i];
        }
        tx_buf_[12] = static_cast<uint8_t>(ethertype >> 8);
        tx_buf_[13] = static_cast<uint8_t>(ethertype & 0xFF);
        for (uint32_t i = 0; i < len; ++i) {
            tx_buf_[14 + i] = l3[i];
        }
        return ctrl_.send_packet(tx_buf_, 14 + len);
    }

private:
    E1000Controller& ctrl_;
    uint8_t          rx_scratch_[1518] = {};  ///< copy-RX destination (reused per poll)
    uint8_t          tx_buf_[1518]     = {};  ///< {EthHdr || L3} build buffer
};

}  // namespace cinux::drivers::net
