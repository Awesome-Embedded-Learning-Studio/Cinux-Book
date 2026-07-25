/**
 * @file kernel/net/raw_socket.cpp
 * @brief RawSocket -- SOCK_RAW + IPPROTO_ICMP adapter (busybox ping path).
 *
 * See raw_socket.hpp.  sendto() hands the user-built ICMP message straight to
 * Ipv4Module::send as the IP payload (Ipv4Module builds the IP header; the user
 * owns the ICMP header + checksum).  recv() dequeues an echo reply that
 * IcmpModule::handle pushed via on_icmp_reply.  Blocking mirrors udp_socket.cpp
 * exactly: an empty ring parks the recv'er on recv_waiters_ via
 * prepare_to_wait/schedule_blocked (lock held only for prepare, the actual
 * schedule_blocked runs LOCK-FREE -- the memory iron rule, lockdep-safe).
 *
 * busybox ping specifics:
 *   - sendto() gets a COMPLETE ICMP packet (type/code/checksum/id/seq + data).
 *     The kernel must NOT add or recompute the ICMP checksum -- the user did.
 *     We pass the bytes straight to Ipv4Module::send (proto=ICMP), which wraps
 *     them in an IP header.  dst_port is ignored (ICMP has no ports) but kept
 *     in the signature to match the Socket virtual + sys_sendto plumbing.
 *   - recv() returns the whole ICMP message (header + data) just like Linux raw
 *     sockets: the reader sees type/code/checksum/id/seq + payload.  out_src is
 *     the echo-reply sender; out_port is 0 (no port).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/raw_socket.hpp"
#include "kernel/net/icmp.hpp"         // IcmpModule (register/unregister)
#include "kernel/net/ipv4.hpp"         // Ipv4Module, kIpProtoIcmp
#include "kernel/net/wait_queue.hpp"   // wake_one / wake_all / wait_enqueue / wait_remove

#include <cstdint>

#ifndef CINUX_HOST_TEST
#    include "kernel/proc/process.hpp"  // Task + signal_deliverable_pending
#    include "kernel/proc/scheduler.hpp"  // prepare_to_wait/schedule_blocked
#endif

namespace cinux::net {

#ifndef CINUX_HOST_TEST
using cinux::proc::Scheduler;
using cinux::proc::Task;
#endif

RawSocket::RawSocket(IcmpModule& icmp, Ipv4Module& ipv4, NetStack& stack, DevRoute route)
    : Socket(kAfInet, kSockRaw), icmp_(icmp), ipv4_(ipv4), stack_(stack), route_(route) {
    // Register so IcmpModule::handle pushes echo replies into our ring.  Marked
    // registered_ so the destructor unregisters exactly once (defensive: a
    // duplicate register is a no-op in IcmpModule anyway).
    icmp_.register_raw_socket(this);
    registered_ = true;
}

RawSocket::~RawSocket() {
    close();  // idempotent: registered_ gates the unregister
}

cinux::lib::ErrorOr<int64_t> RawSocket::sendto(Ipv4Addr dst, uint16_t /*dst_port*/,
                                                const uint8_t* buf, uint32_t len) {
    if (shut_write()) {
        return cinux::lib::Error::BrokenPipe;  // SHUT_WR recorded -> EPIPE
    }
    if (buf == nullptr || len == 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    // Cap to one Ethernet frame minus L2+IPv4 (matches Ipv4Module::send's check).
    uint32_t n = len > kMaxMsg ? kMaxMsg : len;

    // Hand the user-built ICMP message STRAIGHT to the IP layer as the L4
    // payload.  Ipv4Module::send builds the IP header + ARP-resolves the next
    // hop + emits on the route-resolved device.  We do NOT touch the ICMP bytes
    // -- busybox already laid out type/code/checksum/id/seq + data.  Compare
    // UdpModule::send (which builds a UDP header); raw has no L4 header to add.
    NetDevice& dev = route_(dst);
    auto       r   = ipv4_.send(dev, dst, kIpProtoIcmp, buf, n, stack_);
    if (!r.ok()) {
        return r.error();
    }
    return static_cast<int64_t>(n);
}

cinux::lib::ErrorOr<int64_t> RawSocket::recv(uint8_t* buf, uint32_t len, Ipv4Addr* out_src,
                                             uint16_t* out_port) {
    if (shut_read()) {
        return static_cast<int64_t>(0);  // SHUT_RD recorded -> EOF (0 bytes)
    }
    if (buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    for (;;) {
#ifndef CINUX_HOST_TEST
        bool need_block = false;
#endif
        {
            auto g = lock_.irq_guard();
            if (rx_count_ > 0) {
                Datagram& dg = rx_[rx_head_];
                uint32_t  n  = len < dg.len ? len : dg.len;
                for (uint32_t i = 0; i < n; ++i) {
                    buf[i] = dg.data[i];
                }
                if (out_src != nullptr) {
                    *out_src = dg.src;
                }
                if (out_port != nullptr) {
                    *out_port = 0;  // ICMP has no ports
                }
                rx_head_ = (rx_head_ + 1) % kRxSlots;
                --rx_count_;
                return static_cast<int64_t>(n);  // one message per recv (truncated)
            }
#ifdef CINUX_HOST_TEST
            return cinux::lib::Error::WouldBlock;  // host: no scheduler, never block
#else
            Task* self = Scheduler::current();
            if (self == nullptr) {
                return cinux::lib::Error::WouldBlock;  // no scheduler context (early boot)
            }
            wait_enqueue(recv_waiters_, self);
            Scheduler::prepare_to_wait(self);
            need_block = true;
#endif
        }
#ifndef CINUX_HOST_TEST
        if (need_block) {
            Scheduler::schedule_blocked();  // LOCK-FREE: lock_ scope exited above
        }
        // EINTR: a signal landed while parked.  Return the sentinel -1 (a value
        // a real byte-count can never take) so the syscall layer maps it to
        // -EINTR.  Unlink ourselves under lock_ first so a producer does not
        // wake a stale link.
        if (Scheduler::current() != nullptr &&
            signal_deliverable_pending(Scheduler::current())) {
            auto g = lock_.irq_guard();
            wait_remove(recv_waiters_, Scheduler::current());
            return static_cast<int64_t>(-1);  // sentinel: sys_recvfrom -> -EINTR
        }
        // Woken by on_icmp_reply() enqueuing a reply; loop and dequeue.
#endif
    }
}

void RawSocket::on_icmp_reply(const Ipv4Header& ip, FrameView payload) {
    // Called from IcmpModule::handle (L4 dispatch, single-threaded per poll).
    // Copy the whole ICMP message (header + data) under lock_, then wake a
    // blocked recv.  Ring-full -> drop (no flow control yet, matches UdpSocket).
    {
        auto g = lock_.irq_guard();
        if (rx_count_ < kRxSlots) {
            Datagram& dg = rx_[rx_tail_];
            dg.src       = ip.src;
            uint32_t n   = payload.size() < kMaxMsg ? static_cast<uint32_t>(payload.size()) : kMaxMsg;
            dg.len       = static_cast<uint16_t>(n);
            for (uint32_t i = 0; i < n; ++i) {
                dg.data[i] = payload[i];
            }
            rx_tail_ = (rx_tail_ + 1) % kRxSlots;
            ++rx_count_;
        }
    }
#ifndef CINUX_HOST_TEST
    wake_one(recv_waiters_);  // a blocked recv can now dequeue
#endif
}

void RawSocket::close() {
    // Unregister FIRST so IcmpModule stops handing us replies, THEN wake any
    // blocked recv'ers (they retry -> empty ring -> WouldBlock -> return).
    // Idempotent: registered_ gates the unregister so double-close is safe.
    if (registered_) {
        icmp_.unregister_raw_socket(this);
        registered_ = false;
    }
#ifndef CINUX_HOST_TEST
    wake_all(recv_waiters_);
#endif
}

uint32_t RawSocket::poll_events([[maybe_unused]] cinux::proc::Task* waiter, bool* registered) {
    auto g = lock_.irq_guard();
    if (registered != nullptr) {
        *registered = (waiter != nullptr);
    }
    uint32_t mask = 0;
    if (rx_count_ > 0) {
        mask |= cinux::fs::kPollIn;  // a reply is queued -> readable
    }
    mask |= cinux::fs::kPollOut;  // raw can generally send (no send-block yet)
#ifndef CINUX_HOST_TEST
    // Park on the same queue a blocked recv uses, so an incoming reply
    // (on_icmp_reply -> wake_one) wakes the poller too.
    if (waiter != nullptr) {
        wait_enqueue(recv_waiters_, waiter);
    }
#endif
    return mask;
}

void RawSocket::poll_detach_waiter([[maybe_unused]] cinux::proc::Task* waiter) {
#ifndef CINUX_HOST_TEST
    auto g = lock_.irq_guard();
    wait_remove(recv_waiters_, waiter);
#endif
}

}  // namespace cinux::net
