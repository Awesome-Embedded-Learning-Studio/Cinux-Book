/**
 * @file kernel/net/net_stack.cpp
 * @brief NetStack dispatch: attach/add_protocol/poll/send_l3/config_for.
 *
 * poll() is the heart: drain one frame per device, parse L2 (Ethernet IF
 * has_ethernet_header, else raw L3 with ethertype from the IPv4 version nibble),
 * dispatch by ethertype, and recycle the buffer on EVERY exit path via a scope
 * guard (the lifetime contract -- a device-owned buffer is never leaked, whether
 * the frame was handled, dropped, or a runt).
 *
 * Zero kprintf by design: this is a pure protocol library, host-linkable with
 * no kernel runtime dependency (logging lives in net_init / the driver adapter).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/net_stack.hpp"

#include <cinux/scope_guard.hpp>

#include "kernel/net/protocol_handler.hpp"

namespace cinux::net {

namespace {

/// Per-poll() frame budget: bounds work so a runaway self-sending handler (or a
/// huge RX burst) cannot hang the pump. Frames that lose the budget stay in the
/// device ring and drain on the next poll() call. 64 is ample for ping and a
/// full loopback request->reply round-trip in a single poll().
constexpr uint32_t kPollBudget = 64;

/// @brief Parse L2 metadata + locate the L3 payload inside a received frame.
/// @return false for a runt / an unhandled no-L2 frame (caller drops, scope
///         guard still recycles the buffer).
bool parse_l2(const NetDevice& dev, const Packet& pkt, L2Info& l2, FrameView& payload) {
    if (dev.has_ethernet_header()) {
        if (pkt.len < sizeof(EthHdr)) {
            return false;  // runt Ethernet frame
        }
        EthHdr eth;
        parse_eth(pkt.data, eth);
        l2.has_ethernet = true;
        l2.dst          = eth.dst;
        l2.src          = eth.src;
        l2.ethertype    = eth.ethertype;
        payload         = FrameView(pkt.data + sizeof(EthHdr), pkt.len - sizeof(EthHdr));
        return true;
    }
    // No L2 (loopback): the whole frame IS the L3 payload.  Derive the ethertype
    // from the IP version nibble (only IPv4 is wired today).
    l2.has_ethernet = false;
    if (pkt.len < 1) {
        return false;
    }
    const uint8_t ver = static_cast<uint8_t>(pkt.data[0] >> 4);
    if (ver == 4) {
        l2.ethertype = kEtherTypeIpv4;
        payload      = FrameView(pkt.data, pkt.len);
        return true;
    }
    return false;  // non-IPv4 on a no-L2 device -> drop
}

}  // namespace

bool NetStack::attach(NetDevice& dev, const InDevice& cfg) {
    // Idempotent per device pointer: re-attach updates the config in place.
    for (uint32_t i = 0; i < kMaxDevs; ++i) {
        if (devs_[i].dev == &dev) {
            devs_[i].cfg = cfg;
            return true;
        }
    }
    for (uint32_t i = 0; i < kMaxDevs; ++i) {
        if (devs_[i].dev == nullptr) {
            devs_[i].dev = &dev;
            devs_[i].cfg = cfg;
            return true;
        }
    }
    return false;  // device list full
}

void NetStack::add_protocol(uint16_t ethertype, ProtocolHandler& h) {
    for (uint32_t i = 0; i < kMaxProtos; ++i) {
        if (protos_[i].h != nullptr && protos_[i].ethertype == ethertype) {
            protos_[i].h = &h;  // replace existing handler for this ethertype
            return;
        }
    }
    for (uint32_t i = 0; i < kMaxProtos; ++i) {
        if (protos_[i].h == nullptr) {
            protos_[i].ethertype = ethertype;
            protos_[i].h         = &h;
            return;
        }
    }
    // table full -- ignore (kMaxProtos covers ARP + IPv4 + headroom)
}

void NetStack::poll() {
    // Drain pending frames round-robin across devices, bounded by kPollBudget so
    // a runaway self-sending handler cannot hang the pump. A single poll() thus
    // completes a loopback request->reply round-trip (the reply is enqueued by
    // send_l3 during dispatch and drained on the next round). Frames that lose
    // the budget stay in the device ring and drain on the next poll() call.
    for (uint32_t budget = kPollBudget; budget > 0; --budget) {
        bool got = false;
        for (uint32_t d = 0; d < kMaxDevs; ++d) {
            DevSlot& slot = devs_[d];
            if (slot.dev == nullptr) {
                continue;
            }
            Packet pkt;
            if (!slot.dev->poll_rx(pkt) || pkt.len == 0) {
                continue;
            }
            got = true;  // a frame was drained from this device this round
            // Inner scope so the recycle guard fires on EVERY exit below,
            // including a `continue` out to the for-d loop (which unwinds this
            // block first).  Lifetime contract: a device-owned buffer is
            // reclaimed whether the frame was handled, dropped, or a runt.
            {
                SCOPE_EXIT(if (pkt.sink != nullptr) { pkt.sink->recycle(pkt.data); });

                L2Info    l2{};
                FrameView payload;  // default-constructed; parse_l2 assigns on success
                if (!parse_l2(*slot.dev, pkt, l2, payload)) {
                    continue;  // runt / unhandled no-L2 frame -> drop (guard recycles)
                }
                for (uint32_t p = 0; p < kMaxProtos; ++p) {
                    if (protos_[p].h != nullptr && protos_[p].ethertype == l2.ethertype) {
                        protos_[p].h->on_frame(l2, payload, *slot.dev, *this);
                        break;
                    }
                }
                // no ethertype match -> silent drop (the guard still recycles)
            }
        }
        if (!got) {
            break;  // every device idle -> done
        }
    }
}

cinux::lib::ErrorOr<void> NetStack::send_l3(NetDevice& dev, const EthAddr& next_hop,
                                            uint16_t ethertype, const uint8_t* l3, uint32_t len) {
    // The device owns L2 framing (Ethernet header prepend, or verbatim for a
    // no-L2 device).  The stack resolves nothing here -- @p next_hop is already
    // the resolved gateway/requester MAC.
    return dev.send_l3(next_hop, ethertype, l3, len);
}

const InDevice* NetStack::config_for(const NetDevice& dev) const {
    for (uint32_t i = 0; i < kMaxDevs; ++i) {
        if (devs_[i].dev == &dev) {
            return &devs_[i].cfg;
        }
    }
    return nullptr;
}

}  // namespace cinux::net
