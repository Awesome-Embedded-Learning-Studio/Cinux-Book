/**
 * @file kernel/net/net_device.hpp
 * @brief NetDevice -- the L2 seam a NIC implements.
 *
 * A collapsed struct net_device_ops.  A 2nd NIC (rtl8139, virtio-net, loopback)
 * implements EXACTLY this interface and nothing more; the e1000 adapter is a
 * thin forwarder to E1000Controller.  The stack never names E1000Controller --
 * this abstract is the only type it sees.
 *
 * Capability model (FOLD-A + honesty):
 *  - mac()->bool / has_ethernet_header(): a device declares whether it has an
 *    L2 address / carries a 14-byte Ethernet header.  e1000/rtl8139: yes;
 *    loopback: no (the dispatcher then treats the whole frame as L3 payload).
 *  - max_frame(): device-driven MTU (default 1518; loopback 65536).  Not a
 *    hardcoded universal constant.
 *  - supports_zerocopy(): the interface is COPY-BASED today (poll_rx copies or
 *    hands a borrowed buffer; send_l3 copies out).  A future virtio returns
 *    true and the stack MAY route it through an extended borrow path (not built
 *    in F7).  Signals intent; does not pretend the path exists.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/buffer.hpp"
#include "kernel/net/net_types.hpp"

namespace cinux::net {

class NetDevice {
public:
    virtual ~NetDevice() = default;

    /// @brief L2 identity.  Writes 6 octets into @p out and returns true IF the
    ///        device has an L2 address.  Returns false for a no-MAC device
    ///        (loopback).  (FOLD-A.)
    virtual bool mac([[maybe_unused]] EthAddr& out) const {
        return false;
    }

    /// @brief True iff frames this device produces/consumes carry a 14-byte
    ///        Ethernet header.  Default true (e1000/rtl8139); loopback overrides
    ///        to false -- the dispatcher then parses raw L3 directly.  (FOLD-A.)
    virtual bool has_ethernet_header() const { return true; }

    /// @brief Link up (informational).  Default true.
    virtual bool link_up() const { return true; }

    /// @brief Maximum frame length the device can deliver/accept, INCLUDING the
    ///        L2 header when has_ethernet_header().  Device-driven, universal
    ///        only by default.  Default 1518; loopback 65536.
    virtual uint32_t max_frame() const { return 1518; }

    /// @brief Capability hint.  The interface is copy-based today; a future
    ///        zero-copy device (virtio-net) returns true.  Default false.
    virtual bool supports_zerocopy() const { return false; }

    /// @brief Poll one received frame.  If a frame is pending, fill @p out
    ///        (data/len/sink) and return true.  False when idle.
    ///
    /// COPY devices set out.data to their internal scratch (sink == nullptr,
    /// recycle is a no-op) and overwrite it on the next poll_rx.  ZERO-COPY
    /// devices set out.data to a borrowed buffer (sink == this) and reclaim it
    /// in BufferSink::recycle().  Either way NetStack::poll() recycles after
    /// dispatch via the scope guard.
    virtual bool poll_rx(Packet& out) = 0;

    /// @brief L3 TX entry point -- the device owns L2 framing.
    ///
    /// For an Ethernet device: prepend a 14-byte EthHdr {next_hop, src=mac(),
    /// ethertype} then transmit {EthHdr || l3_payload}.  For a no-L2 device:
    /// transmit l3_payload verbatim (@p next_hop ignored).  The device COPIES
    /// into its own TX ring (e1000::send_packet does), so @p l3_payload is dead
    /// on return -- it need NOT be DMA-able.  @p next_hop is the resolved
    /// gateway/requester MAC (the stack resolved it via ARP before calling).
    ///
    /// @p ethertype is HOST order (kEtherType*); the device serialises it to
    /// wire (hi byte first).
    virtual cinux::lib::ErrorOr<void> send_l3(const EthAddr& next_hop, uint16_t ethertype,
                                              const uint8_t* l3, uint32_t len) = 0;
};

}  // namespace cinux::net
