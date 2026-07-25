/**
 * @file kernel/net/protocol_handler.hpp
 * @brief ProtocolHandler -- the L3 seam a protocol implements.
 *
 * One protocol per ethertype (ARP=0x0806, IPv4=0x0800) -- Linux dev_add_pack /
 * ptype_base stripped to "handle this payload".  The handler receives the L3
 * payload (already PAST any L2 header; for a no-L2 device, the whole frame),
 * the L2 metadata bundle, the device the frame arrived on (FOLD-B: so a reply
 * egresses the SAME NIC), and the stack (to enqueue replies).  Return ignored
 * -- a protocol replies or drops silently, like Linux.
 *
 * Namespace: cinux::net
 */

#pragma once

#include "kernel/net/net_types.hpp"

namespace cinux::net {

class NetDevice;  // forward -- see net_device.hpp
class NetStack;   // forward -- see net_stack.hpp

class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;

    /// @brief Handle one inbound L3 payload.
    /// @param l2      parsed L2 metadata (src/dst MAC + ethertype, or
    ///                has_ethernet==false for a no-L2 device).
    /// @param payload L3 bytes (ARP body / IPv4 packet / whole frame if no L2).
    ///                Valid ONLY for the duration of this call -- copy to retain.
    /// @param dev     device the frame arrived on -- reply on THIS one.  (FOLD-B.)
    /// @param stack   for sending replies (stack.send_l3(dev, ...)).
    virtual void on_frame(const L2Info& l2, FrameView payload, NetDevice& dev, NetStack& stack) = 0;
};

}  // namespace cinux::net
