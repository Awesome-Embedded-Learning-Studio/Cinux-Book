/**
 * @file    tcp.hpp
 * @brief   The TCP module: connections, handshakes, and honest bytes.
 *
 * TCP earns its keep where UDP shrugged: the module owns one state
 * machine per connection, a table of fixed seats, and the discipline
 * that every byte is counted. Incoming segments are judged four times
 * before any state moves — the word book parses the header, the data
 * offset must sit inside the bytes that arrived, the checksum over the
 * pseudo header plus the whole segment must fold to zero (TCP sends no
 * zero-skip excuse), and the sequence space must agree with what the
 * connection expects — and every refusal lands in its own counter.
 * What survives drives the machine: handshake, in-order delivery into
 * a per-connection inbox, acknowledgement of what was consumed, and
 * the four-part farewell. News reaches the socket layer through the
 * caller's three callbacks, and they never fire under the module's
 * gate: the segment's work is done first, the callback after, so a
 * callback that walks back in through receive() cannot deadlock the
 * module it is calling.
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
#include "cinux/result.hpp"
#include "cinux/spin_mutex.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/time/timer_table.hpp"

namespace cinux::net {

/**
 * @brief   Where one connection stands in the protocol's own story.
 * @since   0.7.0
 * @ingroup kernel_net
 */
enum class TcpState : uint8_t {
    /// The seat is free.
    kClosed,
    /// Active open: a SYN left, no answer yet.
    kSynSent,
    /// Passive open: the peer's SYN arrived, this side's SYN is away.
    kSynReceived,
    /// Both directions open, bytes may travel.
    kEstablished,
    /// This side said goodbye, the FIN is unacknowledged.
    kFinWait1,
    /// This side's FIN is acknowledged, the peer may still speak.
    kFinWait2,
    /// The peer said goodbye, this side may still speak.
    kCloseWait,
    /// Both FINs are away, neither acknowledged.
    kClosing,
    /// The passive closer's FIN is away, awaiting its acknowledgement.
    kLastAck,
    /// Both directions closed, lingering for stragglers.
    kTimeWait,
};

/**
 * @brief   Why a connection ended, as the socket layer needs to know it.
 * @since   0.7.0
 * @ingroup kernel_net
 */
enum class TcpFarewell : uint8_t {
    /// Both sides finished the farewell dance.
    kGraceful,
    /// A reset killed the connection.
    kReset,
    /// The peer stopped answering and the stack gave up.
    kUnreachable,
};

/**
 * @brief   What a socket wants to hear about, one pointer per news kind.
 * @note    Every callback runs after the module's gate is released, so
 *          a callback may call back into the module.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct TcpEvents {
    /// Shared context handed back with every callback.
    void* context                                         = nullptr;
    /// Readable bytes arrived.
    void (*data)(void* context)                           = nullptr;
    /// A listener's handshake completed; the connection may be taken.
    void (*accepted)(void* context)                       = nullptr;
    /// The connection ended for good; the ticket is dead after this.
    void (*farewell)(void* context, TcpFarewell farewell) = nullptr;
};

/**
 * @brief   One connection's handle: a seat number and the seat's generation.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct TcpTicket {
    /// Which seat in the connection table.
    unsigned long slot  = 0;
    /// The seat's generation when the ticket was minted.
    unsigned long stamp = 0;
};

/**
 * @brief   One listener's handle: its own table, its own generations.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct TcpListenTicket {
    /// Which seat in the listener table.
    unsigned long slot  = 0;
    /// The seat's generation when the ticket was minted.
    unsigned long stamp = 0;
};

/**
 * @brief   The delivery, refusal, and teardown counters so far.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct TcpStats {
    /// Segments this module built and handed to the sink.
    unsigned long segments_sent          = 0;
    /// Payload bytes appended to some connection's inbox.
    unsigned long bytes_delivered        = 0;
    /// Passively opened connections that reached established.
    unsigned long accepted               = 0;
    /// Segments the word book refused as too short.
    unsigned long refused_short          = 0;
    /// Segments whose data offset was dishonest for the bytes held.
    unsigned long refused_offset         = 0;
    /// Segments whose checksum did not fold to zero.
    unsigned long refused_checksum       = 0;
    /// Handshakes refused because no connection seat was free.
    unsigned long refused_full           = 0;
    /// Segments naming no connection and no listener, answered with RST.
    unsigned long unclaimed              = 0;
    /// Whole segments that arrived below the read cursor, re-answered.
    unsigned long stale_dropped          = 0;
    /// Segments that arrived ahead of a gap, re-answered for batch three.
    unsigned long out_of_order_dropped   = 0;
    /// Acknowledgements naming bytes the connection never sent.
    unsigned long ack_beyond_dropped     = 0;
    /// Reset segments this module sent.
    unsigned long resets_sent            = 0;
    /// Reset segments that killed a connection of this stack.
    unsigned long resets_received        = 0;
    /// Segments re-sent because the wait for their acknowledgement ran out.
    unsigned long retransmits_sent       = 0;
    /// Times a retransmission wait expired on some connection.
    unsigned long rto_timeouts           = 0;
    /// Connections given up after spending every retransmission.
    unsigned long giveups                = 0;
    /// Oldest segments re-sent on three duplicate acknowledgements.
    unsigned long fast_retransmits       = 0;
    /// Out-of-order segments taken into the repair seats.
    unsigned long out_of_order_stored    = 0;
    /// Repair-seat segments delivered once their gap filled.
    unsigned long out_of_order_recovered = 0;
};

/**
 * @brief   The TCP module, a plain instance the wiring owns.
 * @note    The connection table is fixed at eight seats and the listener
 *          table at four; exhaustion is refused by name, never queued.
 * @since   0.7.0
 * @ingroup kernel_net
 */
class TcpModule {
public:
    /**
     * @brief         Aims the module at its sink and source address.
     *
     * @param[in]     sink          Where built segments go.
     * @param[in]     own_address   Source address the pseudo header sums.
     * @return        None
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void init(const DatagramSink& sink, uint32_t own_address);

    /**
     * @brief         Re-aims the module without forgetting anything.
     *
     * @param[in]     sink          The new outgoing path.
     * @param[in]     own_address   The new source address.
     * @return        None
     * @note          For the boot wiring's reroute: the loopback world
     *                runs first, a wire joins later, and the connection
     *                table and counters survive the aim.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void aim(const DatagramSink& sink, uint32_t own_address);

    /**
     * @brief         Judges one segment and drives the machine it names.
     *
     * @param[in]     bytes   First segment byte.
     * @param[in]     length  Segment bytes, payload included.
     * @param[in]     header  The IPv4 header it travelled in.
     * @return        None
     * @note          The service pass owns the clock: an
     *                acknowledgement that clears every unacknowledged
     *                byte measures its round trip against the module's
     *                last service time.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length, const Ipv4View& header);

    /**
     * @brief         Opens a listener under a port, passively.
     *
     * @param[in]     port      The port handshakes must name.
     * @param[in]     events    Where the listener's news goes.
     * @return        The listener's ticket, or the refusal.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    base::Result<TcpListenTicket, base::KernelError> listen(uint16_t port, const TcpEvents& events);

    /**
     * @brief         Closes a listener; seated connections live on.
     *
     * @param[in]     ticket   The ticket listen() answered.
     * @return        True when a listener sat there; false for a stale
     *                ticket.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    bool unlisten(const TcpListenTicket& ticket);

    /**
     * @brief         Takes the ticket of one completed handshake.
     *
     * @param[in]     ticket   The listener's ticket.
     * @return        The accepted connection's ticket, or kWouldBlock
     *                when the queue is empty.
     * @note          The accepted callback only rings the bell; the
     *                connection's ticket waits in the listener's queue
     *                until this call takes it.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    base::Result<TcpTicket, base::KernelError> take_accepted(const TcpListenTicket& ticket);

    /**
     * @brief         Opens a connection to a remote port, actively.
     *
     * @param[in]     local_port       The port to speak from, zero for an
     *                                 ephemeral pick.
     * @param[in]     remote_address   Address to open against.
     * @param[in]     remote_port      Port to open against.
     * @param[in]     events           Where the connection's news goes.
     * @param[in]     now              The monotonic now, in nanoseconds.
     * @return        The connection's ticket, or the refusal.
     * @note          The SYN leaves at once; the connection stands in
     *                syn-sent until the answer arrives, and the refusal
     *                of a refused open surfaces through the farewell
     *                callback, not through this return value. The now
     *                starts the SYN's retransmission clock.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    base::Result<TcpTicket, base::KernelError> connect(uint16_t local_port, uint32_t remote_address,
                                                       uint16_t           remote_port,
                                                       const TcpEvents&   events,
                                                       unsigned long long now);

    /**
     * @brief         Offers payload bytes to the connection to send.
     *
     * @param[in]     ticket   The connection's ticket.
     * @param[in]     payload  First payload byte.
     * @param[in]     length   Payload bytes offered.
     * @param[in]     now      The monotonic now, in nanoseconds.
     * @return        How many bytes the connection took, or the refusal.
     * @note          Takes only what the unacknowledged ceiling allows,
     *                split into honest segments; a short take means the
     *                rest waits for acknowledgements to open room. Every
     *                taken byte stays in the connection's retention ring
     *                until its acknowledgement, and the now starts the
     *                ring's retransmission clock when the ring was empty.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    base::Result<size_t, base::KernelError> send(const TcpTicket&     ticket,
                                                 const unsigned char* payload, size_t length,
                                                 unsigned long long now);

    /**
     * @brief         Takes up to capacity bytes from the connection's inbox.
     *
     * @param[in]     ticket    The connection's ticket.
     * @param[out]    into      Where the bytes land.
     * @param[in]     capacity  How many bytes into may hold.
     * @return        The bytes taken, zero at end-of-file, or the refusal.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    base::Result<size_t, base::KernelError> receive(const TcpTicket& ticket, unsigned char* into,
                                                    size_t capacity);

    /**
     * @brief         Whether the peer's farewell has arrived.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        True once the peer's FIN was consumed; a drained
     *                inbox past that point reads as end-of-file.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool concluded(const TcpTicket& ticket) const;

    /**
     * @brief         Whether the connection may carry bytes both ways.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        True in the established state.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool established(const TcpTicket& ticket) const;

    /// True only while a live ticket is waiting for handshake segments.
    [[nodiscard]] bool connecting(const TcpTicket& ticket) const;

    /**
     * @brief         Begins the graceful farewell for the local direction.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        None
     * @note          Sends the FIN and moves the machine on; the seat
     *                frees itself when the dance completes. A connection
     *                already closing simply ignores the call.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void close(const TcpTicket& ticket);

    /**
     * @brief         Kills the connection at once with a reset.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        None
     * @note          For the forced path: the seat frees immediately
     *                and the farewell callback answers with kReset.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void abandon(const TcpTicket& ticket);

    /**
     * @brief         Advances every connection's clock work by one step.
     *
     * @param[in]     now   The monotonic now, in nanoseconds.
     * @return        None
     * @note          The wiring calls this from its heartbeat. One pass
     *              arms a retransmission guard for every connection with
     *                bytes in flight; the timer table fires the guards
     *                whose wait ran out, and the callback re-sends the
     *                oldest unacknowledged segment, backs the wait off,
     *                or gives the connection up once its retries are
     *                spent.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    void service(unsigned long long now);

    /**
     * @brief         The delivery and refusal counters so far.
     * @return        A reference to the current statistics.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const TcpStats& stats() const;

    /**
     * @brief         The monotonic now of the last service pass.
     * @return        Nanoseconds on the service clock.
     * @note          The face callers without their own clock use:
     *                the wiring refreshes it every pump, so it stands
     *                within one pass of the true now.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] unsigned long long clock() const;

    /**
     * @brief         The source address the pseudo header sums with.
     * @return        The module's own address.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] uint32_t own_address() const;

    /**
     * @brief         Whether the connection offers a reader progress.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        True when bytes wait, the peer said goodbye, or
     *                the seat itself is gone — all three read as news.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool holds_bytes(const TcpTicket& ticket) const;

    /**
     * @brief         Names the far end one connection speaks with.
     *
     * @param[in]     ticket    The connection's ticket.
     * @param[out]    address   The peer's address lands here.
     * @param[out]    port      The peer's port lands here.
     * @return        True on a live connection.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool peer_of(const TcpTicket& ticket, uint32_t& address, uint16_t& port) const;

    /**
     * @brief         Whether the ticket still names a seated connection.
     *
     * @param[in]     ticket   The connection's ticket.
     * @return        True while the seat lives; false once released.
     * @note          The blocking open's failure sense: a ticket that
     *                stopped being alive never establishes.
     * @since         0.7.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool alive(const TcpTicket& ticket) const;

private:
    /**
     * @brief   One listening port: the handshake's doorbell.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct ListenSeat {
        bool          used                           = false;
        unsigned long stamp                          = 0;
        uint16_t      port                           = 0;
        TcpEvents     events                         = {};
        TcpTicket     accepted[kTcpAcceptQueueSeats] = {};
        unsigned long accepted_count                 = 0;
    };

    /**
     * @brief   One out-of-order segment parked until its gap fills.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct ReorderSeat {
        bool          used               = false;
        uint32_t      start              = 0;
        unsigned long bytes              = 0;
        unsigned char data[kTcpMssBytes] = {};
    };

    /**
     * @brief   One live connection, the whole transmission-control block.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct Connection {
        unsigned long      stamp            = 0;
        /// When the oldest unacknowledged byte left, in monotonic
        /// nanoseconds; zero while nothing is in flight.
        unsigned long long oldest_sent_ns   = 0;
        /// Retransmissions spent on the current stretch of silence.
        unsigned long      retransmit_count = 0;
        /// Smoothed round-trip estimate, in nanoseconds; zero until a
        /// sample lands.
        unsigned long long srtt_ns          = 0;
        /// Round-trip variation, in nanoseconds; zero until a sample lands.
        unsigned long long rttvar_ns        = 0;
        /// The current retransmission wait, in nanoseconds; zero until
        /// the first need.
        unsigned long long rto_ns           = 0;
        /// The module this connection serves; the timer's callback walks it.
        TcpModule*         owner            = nullptr;
        /// The timer seat guarding this connection's in-flight bytes.
        time::TimerTicket  rto_seat         = {};
        TcpEvents          events           = {};
        uint32_t           remote_address   = 0;
        /// Oldest unacknowledged sequence number on the send side.
        uint32_t           snd_una          = 0;
        /// Next sequence number the send side uses.
        uint32_t           snd_nxt          = 0;
        /// Next sequence number expected from the peer.
        uint32_t           rcv_nxt          = 0;
        /// Send ceiling the peer last advertised; the default until an
        /// acknowledgement names a window.
        uint32_t           snd_window       = static_cast<uint32_t>(kTcpSendWindowBytes);
        /// Bytes received in order, waiting for their reader.
        base::container::RingQueue<unsigned char, kTcpReceiveWindowBytes + 1> inbox  = {};
        /// Bytes sent but unacknowledged, oldest first.
        base::container::RingQueue<unsigned char, kTcpSendWindowBytes + 1>    outbox = {};
        /// Out-of-order segments parked until their gap fills.
        ReorderSeat       reorder[kTcpReorderSeats]                                  = {};
        /// Duplicate acknowledgements naming the same unacknowledged
        /// byte since the send window last moved.
        unsigned long     duplicate_acks                                             = 0;
        uint16_t          remote_port                                                = 0;
        uint16_t          local_port                                                 = 0;
        bool              used                                                       = false;
        TcpState          state             = TcpState::kClosed;
        /// True while the timer table holds a seat for this connection.
        bool              rto_armed         = false;
        /// The timer seat holding this connection's 2MSL linger.
        time::TimerTicket time_wait_seat    = {};
        /// True while the connection lingers in time-wait.
        bool              time_wait_armed   = false;
        /// True from a retransmission until an acknowledgement arrives,
        /// so the round-trip sampler can skip ambiguous stretches.
        bool              in_retransmission = false;
    };

    /**
     * @brief   One piece of news waiting to leave the module's gate.
     * @since   0.7.0
     * @ingroup kernel_net
     */
    struct PendingFire {
        TcpEvents   events   = {};
        bool        data     = false;
        bool        accepted = false;
        bool        ended    = false;
        TcpFarewell farewell = TcpFarewell::kGraceful;
    };

    Connection* find_connection(uint32_t remote_address, uint16_t remote_port, uint16_t local_port);
    Connection* live_connection(const TcpTicket& ticket);
    const Connection*      live_connection(const TcpTicket& ticket) const;
    ListenSeat*            find_listen(uint16_t port);
    Connection*            claim_free_connection();
    uint16_t               pick_local_port();
    [[nodiscard]] uint16_t receive_window_of(const Connection& connection) const;
    void transmit_segment(const Connection& connection, unsigned char flags,
                          const unsigned char* payload, size_t length, uint32_t sequence);
    void transmit_rst(uint32_t remote_address, uint16_t remote_port, uint16_t local_port,
                      uint32_t sequence, uint32_t acknowledgement, bool with_ack);
    void release_connection(Connection& connection, PendingFire& fire, TcpFarewell farewell);
    void absorb_segment(Connection& connection, const TcpView& view, const unsigned char* payload,
                        size_t payload_bytes, size_t segment_length, PendingFire& fire);
    bool absorb_ack(Connection& connection, const TcpView& view, size_t segment_length,
                    PendingFire& fire);
    void answer_unclaimed(const Ipv4View& header, const TcpView& view, size_t segment_length);
    void absorb_bytes(Connection& connection, const TcpView& view, const unsigned char* payload,
                      size_t payload_bytes, size_t segment_length, PendingFire& fire);
    void after_ack_movement(Connection& connection, bool moved, unsigned long long now);
    void store_out_of_order(Connection& connection, const TcpView& view,
                            const unsigned char* payload, size_t payload_bytes);
    void harvest_reorder(Connection& connection, PendingFire& fire);
    void resend_oldest(Connection& connection);
    void take_rtt_sample(Connection& connection, unsigned long long now);
    [[nodiscard]] unsigned long long current_rto_of(const Connection& connection) const;
    void                             arm_rto_guard(Connection& connection);
    void                             disarm_rto_guard(Connection& connection);
    void                             retransmit_due(Connection& connection);
    void                             begin_time_wait(Connection& connection);
    void                             expire_time_wait(Connection& connection);
    static void                      on_time_wait_due(void* context);
    static void                      on_rto_due(void* context);
    static void                      deliver_fire(const PendingFire& fire);

    mutable base::SpinMutex gate_;
    DatagramSink            sink_                               = {};
    uint32_t                own_address_                        = 0;
    ListenSeat              listens_[kTcpListenSeats]           = {};
    Connection              connections_[kTcpConnectionSeats]   = {};
    unsigned long           ephemeral_cursor_                   = kEphemeralPortFirst;
    uint32_t                isn_cursor_                         = 0;
    TcpStats                stats_                              = {};
    /// Farewells the service pass collected, delivered outside the gate.
    PendingFire             service_fires_[kTcpConnectionSeats] = {};
    /// How many seats of service_fires_ hold news.
    unsigned long           service_fire_count_                 = 0;
    /// The monotonic now of the running service pass, in nanoseconds.
    unsigned long long      service_now_ns_                     = 0;
};

}  // namespace cinux::net
