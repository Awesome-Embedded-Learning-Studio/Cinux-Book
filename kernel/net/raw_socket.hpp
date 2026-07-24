/**
 * @file kernel/net/raw_socket.hpp
 * @brief RawSocket -- Socket adapter for SOCK_RAW (IPPROTO_ICMP ping path).
 *
 * busybox ping opens socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) and issues echo
 * requests via sendto(), reading echo replies via recvfrom().  RawSocket is the
 * per-fd adapter that wires that flow onto the existing L3 stack:
 *
 *   - sendto() hands the user-built ICMP message (header + data) STRAIGHT to
 *     Ipv4Module::send as the IP payload (proto=ICMP).  Ipv4Module builds the IP
 *     header; the user owns the ICMP header + checksum (busybox computes it).
 *   - recv() dequeues an inbound ICMP echo reply that IcmpModule::handle pushed
 *     into this socket's RX ring.  IcmpModule walks its registered RawSocket list
 *     on every echo reply and copies the whole ICMP message (header + data) in.
 *
 * Layout mirrors UdpSocket (datagram ring + blocking recv via the shared
 * wait_queue helpers), minus the port demux -- raw ICMP has no ports, so every
 * registered socket receives every echo reply (Linux delivers a COPY to each
 * raw socket bound to the protocol).  No auto-bind, no connect, no listen.
 *
 * The protocol module (IcmpModule) stays pure: per-fd state + buffering live
 * here, in the adapter (CODING-TASTE §13 -- the xHCI/HID antipattern averted).
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/net_device.hpp"  // NetDevice
#include "kernel/net/net_stack.hpp"   // NetStack
#include "kernel/net/socket.hpp"      // Socket
#include "kernel/proc/sync.hpp"       // Spinlock

// Ipv4Header + FrameView (on_icmp_reply signature).  Same include chain
// udp_socket.hpp reaches via udp.hpp -> ipv4.hpp; pulled here directly so the
// header is self-contained.
#include "kernel/net/ipv4.hpp"  // Ipv4Header, FrameView (via net_types.hpp)
#include "kernel/net/icmp.hpp"  // RawListener (echo-reply delivery interface we implement)

namespace cinux::proc {
struct Task;  // forward -- wait queue holds blocked recv'ers (host-guarded)
}

namespace cinux::net {

class Ipv4Module;
class IcmpModule;

/// @brief RAW socket (SOCK_RAW + IPPROTO_ICMP): ICMP ping adapter behind a fd.
///
/// sendto() emits a user-built ICMP message via Ipv4Module::send; recv() reads
/// echo replies IcmpModule pushed into the per-socket ring.  Blocking mirrors
/// UdpSocket: an empty ring parks on a wait queue via prepare_to_wait /
/// schedule_blocked, and the producer (IcmpModule::handle) wake_one()s it.
class RawSocket : public Socket, public RawListener {
public:
    /// Route resolver: pick the egress NetDevice for a destination address.
    /// Same shape as UdpSocket::DevRoute (production routes 127/8 -> loopback,
    /// else -> the preferred NIC).
    using DevRoute = NetDevice& (*)(Ipv4Addr dst);

    /// @brief Construct a raw ICMP socket.
    /// @param icmp  the ICMP module -- the socket registers itself so echo
    ///              replies are pushed into its ring (unregistered on close()).
    /// @param ipv4  for sendto() (Ipv4Module::send builds the IP header).
    /// @param stack for L3 TX + device config lookup.
    /// @param route the egress-device resolver (same as UdpSocket).
    RawSocket(IcmpModule& icmp, Ipv4Module& ipv4, NetStack& stack, DevRoute route);

    ~RawSocket() override;

    // --- Socket overrides ---
    cinux::lib::ErrorOr<int64_t> sendto(Ipv4Addr dst, uint16_t dst_port, const uint8_t* buf,
                                        uint32_t len) override;
    cinux::lib::ErrorOr<int64_t> recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                      uint16_t* out_port) override;
    void                         close() override;

    // --- F8-M5 poll: POLLIN when a reply is queued, else park on recv_waiters_. ---
    uint32_t poll_events(cinux::proc::Task* waiter, bool* registered) override;
    void     poll_detach_waiter(cinux::proc::Task* waiter) override;

    /// @brief Push an inbound ICMP echo reply into the ring (called by
    ///        IcmpModule::handle under its own context).  Copies the whole ICMP
    ///        message (header + data) so the reader sees what the wire carried,
    ///        then wake_one()s a blocked recv.
    /// @param ip      the IPv4 header (src = echo-reply sender).
    /// @param payload the full ICMP message (header + data).  Borrowed: copied.
    void on_icmp_reply(const Ipv4Header& ip, FrameView payload) override;

private:
    static constexpr uint32_t kRxSlots = 8;      ///< queued ICMP messages per socket
    static constexpr uint32_t kMaxMsg  = 1484;   ///< cap (one Ethernet frame minus L2+IPv4)

    struct Datagram {
        Ipv4Addr src{};
        uint16_t len = 0;
        uint8_t  data[kMaxMsg];
    };

    IcmpModule& icmp_;
    Ipv4Module& ipv4_;
    NetStack&   stack_;
    DevRoute    route_;
    bool        registered_ = false;  ///< IcmpModule registration succeeded

    Datagram              rx_[kRxSlots]{};
    uint32_t              rx_head_ = 0, rx_tail_ = 0, rx_count_ = 0;
    cinux::proc::Spinlock lock_;
    cinux::proc::Task*    recv_waiters_ = nullptr;  ///< intrusive list (host-guarded)
};

}  // namespace cinux::net
