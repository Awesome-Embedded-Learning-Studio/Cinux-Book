/**
 * @file kernel/net/udp_socket.hpp
 * @brief UdpSocket -- Socket adapter over UdpModule (F7-M6 batch 2).
 *
 * UdpModule (F7-M4) is a PURE protocol layer: it demuxes inbound datagrams to a
 * UdpListener and emits outbound ones.  UdpSocket IS-A UdpListener: on_udp()
 * COPIES the borrowed payload into a per-socket datagram ring (the device
 * recycles the frame after dispatch), and recv() dequeues.  bind() registers
 * with UdpModule; sendto() calls UdpModule::send on the route-resolved device.
 *
 * Blocking mirrors kernel/ipc/pipe: recv() on an empty ring parks on a wait
 * queue via prepare_to_wait/schedule_blocked; on_udp() wake_one()s it.  #DF-safe
 * (no sti in the syscall path).  Host unit tests compile the blocking path out
 * (CINUX_HOST_TEST) and use a non-block recv / poll-first flow.
 *
 * The protocol module stays pure; per-fd state + buffering live here
 * (CODING-TASTE §13 -- the xHCI/HID antipattern averted).
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/net_device.hpp"  // NetDevice
#include "kernel/net/net_stack.hpp"   // NetStack
#include "kernel/net/socket.hpp"      // Socket
#include "kernel/net/udp.hpp"         // UdpListener, UdpModule
#include "kernel/proc/sync.hpp"       // Spinlock

namespace cinux::proc {
struct Task;  // forward -- wait queue holds blocked recv'ers (host-guarded)
}

namespace cinux::net {

class Ipv4Module;

/// @brief UDP socket: a UdpListener-fed datagram ring behind a Socket fd.
class UdpSocket : public Socket, public UdpListener {
public:
    /// Route resolver: pick the egress NetDevice for a destination address.
    /// Production (net_init) routes 127/8 -> loopback, else -> e1000 adapter;
    /// tests pass a resolver that returns their (loopback) device.
    using DevRoute = NetDevice& (*)(Ipv4Addr dst);

    UdpSocket(UdpModule& udp, Ipv4Module& ipv4, NetStack& stack, DevRoute route);

    // --- Socket overrides ---
    cinux::lib::ErrorOr<void>    bind(uint16_t local_port) override;
    cinux::lib::ErrorOr<void>    connect(Ipv4Addr remote, uint16_t remote_port) override;
    cinux::lib::ErrorOr<int64_t> send(const uint8_t* buf, uint32_t len) override;
    cinux::lib::ErrorOr<int64_t> sendto(Ipv4Addr dst, uint16_t dst_port, const uint8_t* buf,
                                        uint32_t len) override;
    cinux::lib::ErrorOr<int64_t> recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                      uint16_t* out_port) override;
    void                         close() override;

    // --- F-ECO batch 7b: address retrieval (getsockname/getpeername) ---
    bool get_local_addr(SockAddrStorage* out) const override;
    bool get_peer_addr(SockAddrStorage* out) const override;

    // --- UdpListener: the modules push inbound datagrams INTO the ring here ---
    void on_udp(const Ipv4Header& ip, uint16_t src_port, FrameView payload) override;

private:
    static constexpr uint32_t kRxSlots  = 8;     ///< queued datagrams per socket
    static constexpr uint16_t kMaxDgram = 1472;  ///< payload cap (MTU - IPv4/UDP hdrs)

    struct Datagram {
        Ipv4Addr src{};
        uint16_t src_port = 0;
        uint16_t len      = 0;
        uint8_t  data[kMaxDgram];
    };

    UdpModule&  udp_;
    Ipv4Module& ipv4_;
    NetStack&   stack_;
    DevRoute    route_;

    uint16_t local_port_ = 0;
    bool     bound_      = false;
    Ipv4Addr peer_addr_{};
    uint16_t peer_port_ = 0;
    bool     connected_ = false;

    Datagram              rx_[kRxSlots]{};
    uint32_t              rx_head_ = 0, rx_tail_ = 0, rx_count_ = 0;
    cinux::proc::Spinlock lock_;
    cinux::proc::Task*    recv_waiters_ = nullptr;  ///< intrusive list (host-guarded)
};

}  // namespace cinux::net
