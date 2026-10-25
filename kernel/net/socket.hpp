/**
 * @file    socket.hpp
 * @brief   The socket family: one base face, the UDP member first.
 *
 * A socket is a file the way a pipe is one: the descriptor table sees
 * an ordinary FsOps pair, and only the socket-numbered door knows what
 * sits behind it. The base class owns what every member shares — the
 * address pair bookkeeping and the identity counter — and declares the
 * face each protocol fills in: take and give whole datagrams or
 * streams, seat addresses, answer them. The UDP member seats itself on
 * the module's port table and keeps a per-socket receive ring, so two
 * sockets on one port table never see each other's traffic. The pump
 * thread and the sending task share each ring, so the ring travels
 * under a spinlock; delivery copies the borrowed payload before it
 * returns, the station's own hard rule.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cinux/container/ring_queue.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "kernel/net/local_config.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/raw.hpp"
#include "kernel/net/tcp.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {
struct FsOps;
}  // namespace cinux::fs

namespace cinux::net {

/**
 * @brief   The internet address family word, AF_INET.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr int kAfInet = 2;

/**
 * @brief   The stream socket type word, SOCK_STREAM.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr int kSockStream = 1;

/**
 * @brief   The datagram socket type word, SOCK_DGRAM.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr int kSockDgram = 2;

/**
 * @brief   The raw socket type word, SOCK_RAW.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr int kSockRaw = 3;

/**
 * @brief   One internet address: the four-byte host word plus a port.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct SocketAddress {
    /// Host-order address, 0 meaning any.
    uint32_t      address = 0;
    /// Host-order port, 0 meaning none chosen yet.
    uint16_t      port    = 0;
    int           family  = kAfInet;        ///< Address family selecting the remaining fields.
    unsigned long name_bytes{};             ///< Significant local name bytes.
    unsigned char name[kLocalNameBytes]{};  ///< Local name, including an abstract leading zero.
};

/**
 * @brief   What one member of the socket family offers.
 * @note    receive and transmit move whole datagrams for the UDP member;
 *          a stream member reads the same face as its byte window. Both
 *          take and give are synchronous kernel-side calls — the
 *          blocking manners belong to the syscall door above, which
 *          retries on would-block until a signal or a timeout says stop.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class Socket : public base::RefCount<> {
public:
    /**
     * @brief         Takes one datagram into the caller's buffer.
     *
     * @param[out]    sink     First byte of the receiving buffer.
     * @param[in]     count    Bytes the buffer holds.
     * @param[out]    from     Where the sender lived, when the caller
     *                         asked; nullptr to skip the question.
     * @return        The bytes taken, zero allowed, or kWouldBlock when
     *                nothing waits.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<unsigned long> receive(void* sink, unsigned long count,
                                                SocketAddress* from) = 0;

    /**
     * @brief         Gives one datagram to the addressed destination.
     *
     * @param[in]     source        First byte of the payload.
     * @param[in]     count         Payload bytes, zero allowed.
     * @param[in]     destination   Where the datagram goes.
     * @return        The bytes given, always count on success.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<unsigned long> transmit(const void* source, unsigned long count,
                                                 const SocketAddress& destination) = 0;

    /**
     * @brief         Seats the socket on a local address and port.
     *
     * @param[in]     local   The address to answer to; port zero asks
     *                        for the next free ephemeral one.
     * @return        Success, or kAlreadyExists for a taken port.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<void> adopt_local(const SocketAddress& local) = 0;

    /**
     * @brief         Names the one destination later sends assume.
     *
     * @param[in]     peer   The address unnamed sends go to.
     * @return        Success.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<void> adopt_peer(const SocketAddress& peer) = 0;

    /**
     * @brief         Answers the socket's own address.
     *
     * @param[out]    local   The seated address and port.
     * @return        Success.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<void> report_local(SocketAddress& local) const = 0;

    /**
     * @brief         Answers the connected destination.
     *
     * @param[out]    peer   The named destination.
     * @return        Success, or kNotFound when none was named.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<void> report_peer(SocketAddress& peer) const = 0;

    /**
     * @brief         Whether a reader makes progress right now.
     * @return        True when something waits to be received.
     * @note          The poll face: readiness with no side effects, so
     *                the descriptor scan may ask cheaply and often.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] virtual bool holds_input() const = 0;

    /** @brief Reports poll readiness including closure. @param[in] events Requested poll bits.
     * @return Ready poll bits. */
    [[nodiscard]] virtual unsigned short poll_events(unsigned int events) const;

    /** @brief The socket's address family. @return Linux address family number. */
    [[nodiscard]] int family() const { return family_; }

    /**
     * @brief         Offers to answer knocks on the seated port.
     *
     * @param[in]     backlog   Accepted seats kept waiting; the module's
     *                          own ceiling wins when this asks for more.
     * @return        Success, or the refusal.
     * @note          The stream member's face; datagram members answer
     *                kNotSupported.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<void> begin_listen(unsigned long backlog);

    /**
     * @brief         Takes one completed handshake as its own socket.
     *
     * @return        The connected socket, or kWouldBlock when no
     *                handshake waits, or kNotSupported off a stream.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    virtual base::Result<base::Shared<Socket>> accept_one();

    /// Sends EOF while keeping the receiving direction open.
    virtual base::Result<void> shutdown_write();

    /**
     * @brief         Whether the opened handshake still runs.
     * @return        True while the connection sits unestablished.
     * @note          The blocking connect loop asks this between
     *                yields; datagram members never run one.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] virtual bool handshake_pending() const;

    /**
     * @brief         Whether the opened handshake died short of open.
     *
     * @return        True when the connection gave up or was reset
     *                before establishing.
     * @note          The blocking open answers refusal through this;
     *                datagram members never run one.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] virtual bool handshake_failed() const;

    /**
     * @brief         The socket type word this member was born with.
     * @return        kSockDgram, kSockRaw, or a later member's word.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] int type() const { return type_; }

    /**
     * @brief         This socket's distinguishing inode number.
     * @return        A number no other socket shares.
     * @note          Numbers rise from a boot-wide counter, the pipe
     *                convention, so stat answers an honest identity.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] unsigned long identity() const { return identity_; }

    /**
     * @brief   Drops the shared face; the release hook does the rest.
     * @note    Virtual because the shared FsOps table deletes through
     *          the base pointer — the member behind it is the only one
     *          who knows its own full size.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    virtual ~Socket() = default;

protected:
    /**
     * @brief         Records the birth type and identity.
     *
     * @param[in]     socket_type   The type word the caller asked for.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    explicit Socket(int socket_type, int family = kAfInet);

private:
    int           type_     = 0;
    int           family_   = kAfInet;
    unsigned long identity_ = 0;
};

/**
 * @brief   Payload bytes one receive slot holds.
 * @note    The largest UDP payload the stack builds, header excluded,
 *          so no honest datagram finds a short slot.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kUdpSocketPayloadBytes =
    kMaxDatagramBytes - kMinIpv4HeaderBytes - kUdpHeaderBytes;

/**
 * @brief   Datagrams one socket's receive ring keeps before dropping.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kUdpSocketQueueDatagrams = 8;

/**
 * @brief   The UDP member: a seated port and a bounded receive ring.
 * @note    One socket owns one port on the module's table; its delivery
 *          callback copies the borrowed payload into the ring under the
 *          ring's lock, because the pump thread delivers while the
 *          owning task receives. A full ring drops the newcomer and
 *          counts it — datagram discipline, not a failure.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class UdpSocket final : public Socket {
public:
    /**
     * @brief         Binds the socket to the module it serves.
     *
     * @param[in,out] module   The UDP module whose port table this
     *                         socket joins at adopt_local time.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    explicit UdpSocket(UdpModule& module);

    /**
     * @brief   Frees the seated port, if one was taken.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    ~UdpSocket() override;

    base::Result<unsigned long> receive(void* sink, unsigned long count,
                                        SocketAddress* from) override;
    base::Result<unsigned long> transmit(const void* source, unsigned long count,
                                         const SocketAddress& destination) override;
    base::Result<void>          adopt_local(const SocketAddress& local) override;
    base::Result<void>          adopt_peer(const SocketAddress& peer) override;
    base::Result<void>          report_local(SocketAddress& local) const override;
    base::Result<void>          report_peer(SocketAddress& peer) const override;
    [[nodiscard]] bool          holds_input() const override;

    /**
     * @brief         Datagrams the ring dropped for being full.
     * @return        The running count.
     * @note          The observable face of the drop policy, for tests
     *                and the curious shell.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] unsigned long dropped() const;

private:
    static void take(void* context, const UdpDelivery& delivery);

    /**
     * @brief   One waiting datagram: sender plus payload copy.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct Waiting {
        SocketAddress from{};
        unsigned char bytes[kUdpSocketPayloadBytes] = {};
        unsigned long length                        = 0;
    };

    UdpModule&                                                        module_;
    SocketAddress                                                     local_{};
    SocketAddress                                                     peer_{};
    bool                                                              seated_ = false;
    bool                                                              named_  = false;
    mutable proc::Spinlock                                            ring_gate_{};
    base::container::RingQueue<Waiting, kUdpSocketQueueDatagrams + 1> waiting_{};
    unsigned long                                                     dropped_ = 0;
};

/**
 * @brief   The erased file face every socket rides.
 * @return  The static FsOps all sockets share.
 * @note    read and write land in receive and transmit; the node's
 *          opaque field points back at the Socket, so retain, release,
 *          and the readiness question all find their owner. The
 *          descriptor-numbered door reaches deeper faces on its own.
 * @since   0.7.0
 * @ingroup kernel_net
 */
const fs::FsOps* SocketOps();

/** @brief ICMP raw socket receiving complete IPv4 datagrams in a bounded ring. */
class RawSocket : public Socket {
public:
    explicit RawSocket(RawModule& module);
    ~RawSocket() override;
    [[nodiscard]] bool          seated() const { return seated_; }
    base::Result<unsigned long> receive(void* sink, unsigned long count,
                                        SocketAddress* from) override;
    base::Result<unsigned long> transmit(const void* source, unsigned long count,
                                         const SocketAddress& destination) override;
    base::Result<void>          adopt_local(const SocketAddress& local) override;
    base::Result<void>          adopt_peer(const SocketAddress& peer) override;
    base::Result<void>          report_local(SocketAddress& local) const override;
    base::Result<void>          report_peer(SocketAddress& peer) const override;
    [[nodiscard]] bool          holds_input() const override;

private:
    struct Waiting {
        SocketAddress from;
        unsigned long length = 0;
        unsigned char bytes[kMaxDatagramBytes]{};
    };
    static void            take(void* context, const unsigned char* bytes, size_t length,
                                const Ipv4View& header);
    RawModule&             module_;
    mutable proc::Spinlock gate_;
    base::container::RingQueue<Waiting, kUdpSocketQueueDatagrams> waiting_;
    SocketAddress                                                 local_{};
    SocketAddress                                                 peer_{};
    bool                                                          named_  = false;
    bool                                                          seated_ = false;
};

/**
 * @brief   The TCP member: one listener seat or one connection ticket.
 * @note    A fresh socket is bare; adopt_local seats a listener, a
 *          later accept_one mints connected sockets from its queue,
 *          while adopt_peer opens a connection actively. The farewell
 *          callback only raises the dead flag — the reader sees
 *          end-of-file, the writer sees a refusal.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class TcpSocket final : public Socket {
public:
    /**
     * @brief         Binds the socket to the module it serves.
     *
     * @param[in,out] module   The TCP module whose tables this socket
     *                         joins.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    explicit TcpSocket(TcpModule& module);

    /**
     * @brief   Returns the seat: listener or connection.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    ~TcpSocket() override;

    base::Result<unsigned long>        receive(void* sink, unsigned long count,
                                               SocketAddress* from) override;
    base::Result<unsigned long>        transmit(const void* source, unsigned long count,
                                                const SocketAddress& destination) override;
    base::Result<void>                 adopt_local(const SocketAddress& local) override;
    base::Result<void>                 adopt_peer(const SocketAddress& peer) override;
    base::Result<void>                 report_local(SocketAddress& local) const override;
    base::Result<void>                 report_peer(SocketAddress& peer) const override;
    [[nodiscard]] bool                 holds_input() const override;
    base::Result<void>                 begin_listen(unsigned long backlog) override;
    base::Result<base::Shared<Socket>> accept_one() override;
    base::Result<void>                 shutdown_write() override;
    [[nodiscard]] bool                 handshake_pending() const override;
    [[nodiscard]] bool                 handshake_failed() const override;

private:
    TcpModule&      module_;
    SocketAddress   local_{};
    SocketAddress   peer_{};
    TcpListenTicket listener_{};
    TcpTicket       connection_{};
    bool            bound_     = false;
    bool            listening_ = false;
    bool            seated_    = false;
    bool            opened_    = false;
};

}  // namespace cinux::net
