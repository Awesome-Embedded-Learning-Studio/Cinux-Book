#include "kernel/net/tcp.hpp"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <new>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "cinux/result.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/time/timer_table.hpp"

namespace cinux::net {

namespace {

constexpr uint32_t kIsnStep = 0x9E3779B9;

bool seq_before(uint32_t left, uint32_t right) {
    return static_cast<int32_t>(left - right) < 0;
}

bool seq_before_eq(uint32_t left, uint32_t right) {
    return static_cast<int32_t>(left - right) <= 0;
}

bool past_fin(TcpState state) {
    return state == TcpState::kCloseWait || state == TcpState::kClosing ||
           state == TcpState::kTimeWait || state == TcpState::kLastAck;
}

void build_pseudo_header(unsigned char* pseudo, uint32_t source, uint32_t destination,
                         uint32_t segment_bytes) {
    base::WriteWord32Be(pseudo, 0, source);
    base::WriteWord32Be(pseudo, 4, destination);
    pseudo[8] = 0;
    pseudo[9] = kIpv4ProtocolTcp;
    base::WriteWord16Be(pseudo, 10, static_cast<uint16_t>(segment_bytes));
}

void seal_checksum(unsigned char* segment, size_t segment_bytes, uint32_t source,
                   uint32_t destination) {
    unsigned char pseudo[12] = {};
    build_pseudo_header(pseudo, source, destination, segment_bytes);
    base::InternetChecksum builder;
    builder.add(pseudo, sizeof(pseudo));
    builder.add(segment, segment_bytes);
    uint16_t checksum = builder.value();
    if (checksum == 0) {
        checksum = 0xFFFF;
    }
    base::WriteWord16Be(segment, 16, checksum);
}

}  // namespace

void TcpModule::init(const DatagramSink& sink, uint32_t own_address) {
    const auto kGate = gate_.guard();
    sink_            = sink;
    own_address_     = own_address;
    stats_           = {};
}

void TcpModule::aim(const DatagramSink& sink, uint32_t own_address) {
    const auto kGate = gate_.guard();
    sink_            = sink;
    own_address_     = own_address;
}

TcpModule::Connection* TcpModule::find_connection(uint32_t remote_address, uint16_t remote_port,
                                                  uint16_t local_port) {
    for (auto& seat : connections_) {
        if (!seat.used) {
            continue;
        }
        if (seat.remote_address == remote_address && seat.remote_port == remote_port &&
            seat.local_port == local_port) {
            return &seat;
        }
    }
    return nullptr;
}

TcpModule::Connection* TcpModule::live_connection(const TcpTicket& ticket) {
    if (ticket.slot >= kTcpConnectionSeats) {
        return nullptr;
    }
    Connection& seat = connections_[ticket.slot];
    if (!seat.used || seat.stamp != ticket.stamp) {
        return nullptr;
    }
    return &seat;
}

const TcpModule::Connection* TcpModule::live_connection(const TcpTicket& ticket) const {
    if (ticket.slot >= kTcpConnectionSeats) {
        return nullptr;
    }
    const Connection& seat = connections_[ticket.slot];
    if (!seat.used || seat.stamp != ticket.stamp) {
        return nullptr;
    }
    return &seat;
}

TcpModule::ListenSeat* TcpModule::find_listen(uint16_t port) {
    for (auto& seat : listens_) {
        if (seat.used && seat.port == port) {
            return &seat;
        }
    }
    return nullptr;
}

TcpModule::Connection* TcpModule::claim_free_connection() {
    for (auto& seat : connections_) {
        if (seat.used) {
            continue;
        }
        const unsigned long kStamp = seat.stamp + 1;
        // Reconstruct in the table: an assignment from Connection{} creates
        // a ~14 KiB temporary on a 16 KiB kernel stack with this toolchain.
        seat.~Connection();
        new (static_cast<void*>(&seat)) Connection{};
        seat.used  = true;
        seat.stamp = kStamp;
        return &seat;
    }
    return nullptr;
}

uint16_t TcpModule::pick_local_port() {
    for (uint32_t scan = 0; scan <= kEphemeralPortLast - kEphemeralPortFirst; ++scan) {
        const auto kCandidate = static_cast<uint16_t>(
            kEphemeralPortFirst + ((ephemeral_cursor_ - kEphemeralPortFirst + scan) %
                                   (kEphemeralPortLast - kEphemeralPortFirst + 1)));
        bool taken = false;
        for (const auto& seat : listens_) {
            if (seat.used && seat.port == kCandidate) {
                taken = true;
            }
        }
        for (const auto& seat : connections_) {
            if (seat.used && seat.local_port == kCandidate) {
                taken = true;
            }
        }
        if (!taken) {
            ephemeral_cursor_ = kCandidate;
            return kCandidate;
        }
    }
    return 0;
}

uint16_t TcpModule::receive_window_of(const TcpModule::Connection& connection) const {
    const unsigned int kHeld = connection.inbox.count();
    return static_cast<uint16_t>(kTcpReceiveWindowBytes - kHeld);
}

// NOLINTNEXTLINE(readability-function-size) a segment names five honest facts
void TcpModule::transmit_segment(const TcpModule::Connection& connection, unsigned char flags,
                                 const unsigned char* payload, size_t length, uint32_t sequence) {
    if (!sink_.assigned()) {
        return;
    }
    unsigned char outgoing[kMaxDatagramBytes] = {};
    const size_t  kSegmentBytes               = kTcpHeaderBytes + length;
    base::WriteWord16Be(outgoing, 0, connection.local_port);
    base::WriteWord16Be(outgoing, 2, connection.remote_port);
    base::WriteWord32Be(outgoing, 4, sequence);
    base::WriteWord32Be(outgoing, 8, connection.rcv_nxt);
    outgoing[12] = static_cast<unsigned char>((kTcpHeaderBytes / sizeof(uint32_t)) << 4U);
    outgoing[13] = flags;
    base::WriteWord16Be(outgoing, 14, receive_window_of(connection));
    base::WriteWord16Be(outgoing, 16, 0);
    base::WriteWord16Be(outgoing, 18, 0);
    for (size_t index = 0; index < length; ++index) {
        outgoing[kTcpHeaderBytes + index] = payload[index];
    }
    seal_checksum(outgoing, kSegmentBytes,
                  SourceAddressFor(own_address_, connection.remote_address),
                  connection.remote_address);
    const OutboundDatagram kDatagram = {
        .destination = connection.remote_address,
        .protocol    = kIpv4ProtocolTcp,
        .payload     = outgoing,
        .length      = kSegmentBytes,
    };
    sink_.send(sink_.context, kDatagram);
    ++stats_.segments_sent;
}

// NOLINTNEXTLINE(readability-function-size) a reset names both sides and both sequence numbers
void TcpModule::transmit_rst(uint32_t remote_address, uint16_t remote_port, uint16_t local_port,
                             uint32_t sequence, uint32_t acknowledgement, bool with_ack) {
    if (!sink_.assigned()) {
        return;
    }
    unsigned char outgoing[kTcpHeaderBytes] = {};
    unsigned char flags                     = kTcpFlagRst;
    if (with_ack) {
        flags |= kTcpFlagAck;
    }
    base::WriteWord16Be(outgoing, 0, local_port);
    base::WriteWord16Be(outgoing, 2, remote_port);
    base::WriteWord32Be(outgoing, 4, sequence);
    base::WriteWord32Be(outgoing, 8, acknowledgement);
    outgoing[12] = static_cast<unsigned char>((kTcpHeaderBytes / sizeof(uint32_t)) << 4U);
    outgoing[13] = flags;
    base::WriteWord16Be(outgoing, 14, 0);
    base::WriteWord16Be(outgoing, 16, 0);
    base::WriteWord16Be(outgoing, 18, 0);
    seal_checksum(outgoing, kTcpHeaderBytes, SourceAddressFor(own_address_, remote_address),
                  remote_address);
    const OutboundDatagram kDatagram = {
        .destination = remote_address,
        .protocol    = kIpv4ProtocolTcp,
        .payload     = outgoing,
        .length      = kTcpHeaderBytes,
    };
    sink_.send(sink_.context, kDatagram);
    ++stats_.resets_sent;
}

void TcpModule::release_connection(TcpModule::Connection& connection, PendingFire& fire,
                                   TcpFarewell farewell) {
    disarm_rto_guard(connection);
    if (connection.time_wait_armed) {
        time::TimerTable::self().cancel(connection.time_wait_seat);
        connection.time_wait_armed = false;
    }
    fire.events                = connection.events;
    fire.ended                 = true;
    fire.farewell              = farewell;
    const unsigned long kStamp = connection.stamp + 1;
    connection.~Connection();
    new (static_cast<void*>(&connection)) Connection{};
    connection.stamp = kStamp;
}

void TcpModule::handle(const unsigned char* bytes, size_t length, const Ipv4View& header) {
    const auto kView = ParseTcp(bytes, length);
    if (!kView.ok()) {
        if (kView.error() == TcpError::kSegmentTooShort) {
            ++stats_.refused_short;
        } else {
            ++stats_.refused_offset;
        }
        return;
    }
    unsigned char pseudo[12] = {};
    build_pseudo_header(pseudo, header.source, header.destination, static_cast<uint32_t>(length));
    base::InternetChecksum verifier;
    verifier.add(pseudo, sizeof(pseudo));
    verifier.add(bytes, length);
    if (verifier.value() != 0) {
        ++stats_.refused_checksum;
        return;
    }

    const TcpView& view           = kView.value();
    const size_t   kPayloadBytes  = length - view.header_bytes;
    const size_t   kSegmentLength = kPayloadBytes +
                                    ((view.flags & kTcpFlagSyn) != 0 ? size_t{1} : size_t{0}) +
                                    ((view.flags & kTcpFlagFin) != 0 ? size_t{1} : size_t{0});

    PendingFire fire{};
    {
        const auto  kGate = gate_.guard();
        Connection* connection =
            find_connection(header.source, view.source_port, view.destination_port);
        if (connection != nullptr && connection->state == TcpState::kTimeWait &&
            (view.flags & kTcpFlagSyn) != 0 && (view.flags & kTcpFlagAck) == 0) {
            PendingFire discard{};
            release_connection(*connection, discard, TcpFarewell::kGraceful);
            answer_unclaimed(header, view, kSegmentLength);
        } else if (connection != nullptr) {
            const uint32_t kUnaBefore = connection->snd_una;
            absorb_segment(*connection, view, bytes + view.header_bytes, kPayloadBytes,
                           kSegmentLength, fire);
            if (connection->used) {
                after_ack_movement(*connection, seq_before(kUnaBefore, connection->snd_una),
                                   service_now_ns_);
            }
        } else {
            answer_unclaimed(header, view, kSegmentLength);
        }
    }
    deliver_fire(fire);
}

// NOLINTNEXTLINE(readability-function-size) an orphan names door, seat, and answer
void TcpModule::answer_unclaimed(const Ipv4View& header, const TcpView& view,
                                 size_t segment_length) {
    if ((view.flags & kTcpFlagSyn) != 0 && (view.flags & kTcpFlagAck) == 0) {
        const ListenSeat* seat  = find_listen(view.destination_port);
        Connection*       fresh = claim_free_connection();
        if (seat == nullptr || fresh == nullptr) {
            if (seat != nullptr) {
                ++stats_.refused_full;
            } else {
                ++stats_.unclaimed;
            }
            if (fresh != nullptr) {
                fresh->used = false;
            }
            transmit_rst(header.source, view.source_port, view.destination_port, 0,
                         view.sequence + static_cast<uint32_t>(segment_length), true);
            return;
        }
        fresh->state          = TcpState::kSynReceived;
        fresh->events         = seat->events;
        fresh->remote_address = header.source;
        fresh->remote_port    = view.source_port;
        fresh->local_port     = view.destination_port;
        fresh->owner          = this;
        fresh->oldest_sent_ns = service_now_ns_;
        fresh->rcv_nxt        = view.sequence + 1;
        const uint32_t kIss   = (isn_cursor_ += kIsnStep);
        fresh->snd_una        = kIss;
        fresh->snd_nxt        = kIss;
        transmit_segment(*fresh, kTcpFlagSyn | kTcpFlagAck, nullptr, 0, fresh->snd_nxt);
        fresh->snd_nxt = kIss + 1;
        return;
    }
    if ((view.flags & kTcpFlagRst) == 0) {
        if ((view.flags & kTcpFlagAck) != 0) {
            transmit_rst(header.source, view.source_port, view.destination_port,
                         view.acknowledgement, 0, false);
        } else {
            transmit_rst(header.source, view.source_port, view.destination_port, 0,
                         view.sequence + static_cast<uint32_t>(segment_length), true);
        }
    }
    ++stats_.unclaimed;
}

// NOLINTNEXTLINE(readability-function-size) the sliding side of one segment
bool TcpModule::absorb_ack(TcpModule::Connection& connection, const TcpView& view,
                           size_t segment_length, PendingFire& fire) {
    if ((view.flags & kTcpFlagAck) != 0) {
        connection.snd_window = view.window;
    }
    if ((view.flags & kTcpFlagAck) != 0 && seq_before(connection.snd_una, view.acknowledgement)) {
        if (seq_before(connection.snd_nxt, view.acknowledgement)) {
            ++stats_.ack_beyond_dropped;
            return true;
        }
        const uint32_t kSpan = view.acknowledgement - connection.snd_una;
        connection.snd_una   = view.acknowledgement;
        for (uint32_t index = 0; index < kSpan; ++index) {
            unsigned char released = 0;
            if (!connection.outbox.pop(released)) {
                break;
            }
        }
        connection.duplicate_acks = 0;
    } else if ((view.flags & kTcpFlagAck) != 0 && view.acknowledgement == connection.snd_una &&
               segment_length == 0 && connection.snd_una != connection.snd_nxt) {
        ++connection.duplicate_acks;
        if (connection.duplicate_acks >= kTcpFastRetransmitAcks) {
            connection.duplicate_acks = 0;
            ++stats_.fast_retransmits;
            resend_oldest(connection);
        }
    }
    if (connection.snd_una == connection.snd_nxt) {
        switch (connection.state) {
        case TcpState::kFinWait1:
            connection.state = TcpState::kFinWait2;
            break;
        case TcpState::kClosing:
            begin_time_wait(connection);
            return true;
        case TcpState::kLastAck:
            release_connection(connection, fire, TcpFarewell::kGraceful);
            return true;
        default:
            break;
        }
    }
    if (segment_length == 0 && view.sequence == connection.rcv_nxt) {
        return true;
    }
    return false;
}

// NOLINTNEXTLINE(readability-function-size) a segment walks one machine
void TcpModule::absorb_segment(TcpModule::Connection& connection, const TcpView& view,
                               const unsigned char* payload, size_t payload_bytes,
                               size_t segment_length, PendingFire& fire) {
    fire.events = connection.events;
    if ((view.flags & kTcpFlagRst) != 0) {
        ++stats_.resets_received;
        release_connection(connection, fire, TcpFarewell::kReset);
        return;
    }

    switch (connection.state) {
    case TcpState::kSynSent:
        if ((view.flags & (kTcpFlagSyn | kTcpFlagAck)) != (kTcpFlagSyn | kTcpFlagAck)) {
            return;
        }
        if (view.acknowledgement != connection.snd_nxt) {
            ++stats_.ack_beyond_dropped;
            return;
        }
        connection.rcv_nxt    = view.sequence + 1;
        connection.snd_una    = view.acknowledgement;
        connection.snd_window = view.window;
        connection.state      = TcpState::kEstablished;
        transmit_segment(connection, kTcpFlagAck, nullptr, 0, connection.snd_nxt);
        return;

    case TcpState::kSynReceived:
        if ((view.flags & kTcpFlagAck) == 0) {
            if ((view.flags & kTcpFlagSyn) != 0 && view.sequence + 1 == connection.rcv_nxt &&
                segment_length == 1) {
                transmit_segment(connection, kTcpFlagSyn | kTcpFlagAck, nullptr, 0,
                                 connection.snd_una);
            }
            return;
        }
        if (view.acknowledgement != connection.snd_nxt) {
            ++stats_.ack_beyond_dropped;
            return;
        }
        connection.snd_una = view.acknowledgement;
        connection.state   = TcpState::kEstablished;
        ++stats_.accepted;
        fire.events   = connection.events;
        fire.accepted = true;
        if (ListenSeat* seat = find_listen(connection.local_port);
            seat != nullptr && seat->accepted_count < kTcpAcceptQueueSeats) {
            seat->accepted[seat->accepted_count] = TcpTicket{
                .slot  = static_cast<unsigned long>(&connection - connections_),
                .stamp = connection.stamp,
            };
            ++seat->accepted_count;
        }
        break;

    case TcpState::kEstablished:
    case TcpState::kFinWait1:
    case TcpState::kFinWait2:
    case TcpState::kCloseWait:
    case TcpState::kClosing:
    case TcpState::kLastAck:
        break;

    case TcpState::kTimeWait:
        transmit_segment(connection, kTcpFlagAck, nullptr, 0, connection.snd_nxt);
        return;

    case TcpState::kClosed:
        return;
    }

    if (absorb_ack(connection, view, segment_length, fire)) {
        return;
    }
    const uint32_t kEnd = view.sequence + static_cast<uint32_t>(segment_length);
    if (seq_before_eq(kEnd, connection.rcv_nxt)) {
        ++stats_.stale_dropped;
        transmit_segment(connection, kTcpFlagAck, nullptr, 0, connection.snd_nxt);
        return;
    }
    if (seq_before(connection.rcv_nxt, view.sequence)) {
        store_out_of_order(connection, view, payload, payload_bytes);
        harvest_reorder(connection, fire);
        transmit_segment(connection, kTcpFlagAck, nullptr, 0, connection.snd_nxt);
        return;
    }
    absorb_bytes(connection, view, payload, payload_bytes, segment_length, fire);
    harvest_reorder(connection, fire);
}

// NOLINTNEXTLINE(readability-function-size) a byte harvest names window and room
void TcpModule::absorb_bytes(TcpModule::Connection& connection, const TcpView& view,
                             const unsigned char* payload, size_t payload_bytes,
                             size_t segment_length, PendingFire& fire) {
    const uint32_t kEnd       = view.sequence + static_cast<uint32_t>(segment_length);
    const size_t   kOffset    = connection.rcv_nxt - view.sequence;
    const size_t kPayloadSkip = kOffset - ((view.flags & kTcpFlagSyn) != 0 ? size_t{1} : size_t{0});
    size_t       taken        = 0;
    if (kPayloadSkip < payload_bytes) {
        const size_t kWanted = payload_bytes - kPayloadSkip;
        const size_t kRoom   = kTcpReceiveWindowBytes - connection.inbox.count();
        taken                = kWanted < kRoom ? kWanted : kRoom;
        for (size_t index = 0; index < taken; ++index) {
            connection.inbox.push(payload[kPayloadSkip + index]);
        }
        connection.rcv_nxt += static_cast<uint32_t>(taken);
        stats_.bytes_delivered += taken;
        if (taken != 0) {
            fire.data = true;
        }
    }
    const bool kProgressed = taken != 0;
    bool       fin_done    = false;
    if ((view.flags & kTcpFlagFin) != 0 && connection.rcv_nxt + 1 == kEnd) {
        connection.rcv_nxt = kEnd;
        switch (connection.state) {
        case TcpState::kEstablished:
        case TcpState::kSynReceived:
            connection.state = TcpState::kCloseWait;
            break;
        case TcpState::kFinWait1:
            connection.state = TcpState::kClosing;
            break;
        case TcpState::kFinWait2:
            connection.state = TcpState::kTimeWait;
            begin_time_wait(connection);
            break;
        default:
            break;
        }
        fin_done = true;
    }
    if (kProgressed || fin_done) {
        transmit_segment(connection, kTcpFlagAck, nullptr, 0, connection.snd_nxt);
    }
}

base::Result<TcpListenTicket, base::KernelError> TcpModule::listen(uint16_t         port,
                                                                   const TcpEvents& events) {
    const auto kGate = gate_.guard();
    for (const ListenSeat& seat : listens_) {
        if (seat.used && seat.port == port) {
            return base::KernelError::kAlreadyExists;
        }
    }
    for (unsigned long index = 0; index < kTcpListenSeats; ++index) {
        ListenSeat& seat = listens_[index];
        if (seat.used) {
            continue;
        }
        const unsigned long kStamp = seat.stamp + 1;
        seat                       = ListenSeat{};
        seat.used                  = true;
        seat.stamp                 = kStamp;
        seat.port                  = port;
        seat.events                = events;
        return TcpListenTicket{.slot = index, .stamp = kStamp};
    }
    return base::KernelError::kOutOfMemory;
}

bool TcpModule::unlisten(const TcpListenTicket& ticket) {
    const auto kGate = gate_.guard();
    if (ticket.slot >= kTcpListenSeats) {
        return false;
    }
    ListenSeat& seat = listens_[ticket.slot];
    if (!seat.used || seat.stamp != ticket.stamp) {
        return false;
    }
    const unsigned long kStamp = seat.stamp + 1;
    seat                       = ListenSeat{};
    seat.stamp                 = kStamp;
    return true;
}

base::Result<TcpTicket, base::KernelError> TcpModule::take_accepted(const TcpListenTicket& ticket) {
    const auto kGate = gate_.guard();
    if (ticket.slot >= kTcpListenSeats) {
        return base::KernelError::kNotFound;
    }
    ListenSeat& seat = listens_[ticket.slot];
    if (!seat.used || seat.stamp != ticket.stamp) {
        return base::KernelError::kNotFound;
    }
    if (seat.accepted_count == 0) {
        return base::KernelError::kWouldBlock;
    }
    const TcpTicket     kTaken = seat.accepted[0];
    const unsigned long kCount = seat.accepted_count;
    std::move(seat.accepted + 1, seat.accepted + kCount, seat.accepted);
    --seat.accepted_count;
    return kTaken;
}

// NOLINTNEXTLINE(readability-function-size) the clock joins the open's four facts
base::Result<TcpTicket, base::KernelError> TcpModule::connect(uint16_t           local_port,
                                                              uint32_t           remote_address,
                                                              uint16_t           remote_port,
                                                              const TcpEvents&   events,
                                                              unsigned long long now) {
    if (remote_address == 0 || remote_port == 0) {
        return base::KernelError::kInvalidArgument;
    }
    const auto  kGate      = gate_.guard();
    Connection* connection = claim_free_connection();
    if (connection == nullptr) {
        return base::KernelError::kOutOfMemory;
    }
    const uint16_t kLocal = local_port != 0 ? local_port : pick_local_port();
    if (kLocal == 0) {
        connection->used = false;
        return base::KernelError::kOutOfMemory;
    }
    connection->state          = TcpState::kSynSent;
    connection->events         = events;
    connection->remote_address = remote_address;
    connection->remote_port    = remote_port;
    connection->local_port     = kLocal;
    connection->owner          = this;
    connection->oldest_sent_ns = now;
    const uint32_t kIss        = (isn_cursor_ += kIsnStep);
    connection->snd_una        = kIss;
    connection->snd_nxt        = kIss;
    transmit_segment(*connection, kTcpFlagSyn, nullptr, 0, connection->snd_nxt);
    connection->snd_nxt = kIss + 1;
    const auto kSlot    = static_cast<unsigned long>(connection - connections_);
    return TcpTicket{.slot = kSlot, .stamp = connection->stamp};
}

base::Result<size_t, base::KernelError> TcpModule::send(const TcpTicket&     ticket,
                                                        const unsigned char* payload, size_t length,
                                                        unsigned long long now) {
    const auto  kGate      = gate_.guard();
    Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return base::KernelError::kNotFound;
    }
    if (connection->state == TcpState::kSynSent || connection->state == TcpState::kSynReceived) {
        return base::KernelError::kWouldBlock;
    }
    if (connection->state != TcpState::kEstablished && connection->state != TcpState::kCloseWait) {
        return base::KernelError::kBrokenPipe;
    }
    const uint32_t kInFlight = connection->snd_nxt - connection->snd_una;
    const uint32_t kCeiling =
        std::min(static_cast<uint32_t>(kTcpSendWindowBytes), connection->snd_window);
    if (kInFlight >= kCeiling) {
        return static_cast<size_t>(0);
    }
    const size_t kRoom     = kCeiling - kInFlight;
    const size_t kAccepted = std::min(length, kRoom);
    size_t       done      = 0;
    while (done < kAccepted) {
        const size_t kChunk = std::min(kAccepted - done, kTcpMssBytes);
        transmit_segment(*connection, kTcpFlagAck | kTcpFlagPsh, payload + done, kChunk,
                         connection->snd_nxt);
        for (size_t index = 0; index < kChunk; ++index) {
            connection->outbox.push(payload[done + index]);
        }
        connection->snd_nxt += static_cast<uint32_t>(kChunk);
        done += kChunk;
    }
    if (connection->oldest_sent_ns == 0) {
        connection->oldest_sent_ns = now;
    }
    return done;
}

base::Result<size_t, base::KernelError> TcpModule::receive(const TcpTicket& ticket,
                                                           unsigned char* into, size_t capacity) {
    const auto  kGate      = gate_.guard();
    Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return base::KernelError::kNotFound;
    }
    if (connection->inbox.count() == 0) {
        if (past_fin(connection->state)) {
            return static_cast<size_t>(0);
        }
        return base::KernelError::kWouldBlock;
    }
    size_t        taken    = 0;
    const bool    kWasShut = receive_window_of(*connection) == 0;
    unsigned char byte     = 0;
    while (taken < capacity && connection->inbox.pop(byte)) {
        into[taken] = byte;
        ++taken;
    }
    if (kWasShut && taken != 0 && receive_window_of(*connection) != 0) {
        transmit_segment(*connection, kTcpFlagAck, nullptr, 0, connection->snd_nxt);
    }
    return taken;
}

bool TcpModule::concluded(const TcpTicket& ticket) const {
    const auto        kGate      = gate_.guard();
    const Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return false;
    }
    return past_fin(connection->state);
}

bool TcpModule::established(const TcpTicket& ticket) const {
    const auto        kGate      = gate_.guard();
    const Connection* connection = live_connection(ticket);
    return connection != nullptr && connection->state == TcpState::kEstablished;
}

bool TcpModule::connecting(const TcpTicket& ticket) const {
    const auto        kGate      = gate_.guard();
    const Connection* connection = live_connection(ticket);
    return connection != nullptr &&
           (connection->state == TcpState::kSynSent || connection->state == TcpState::kSynReceived);
}

void TcpModule::close(const TcpTicket& ticket) {
    const auto  kGate      = gate_.guard();
    Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return;
    }
    switch (connection->state) {
    case TcpState::kSynSent:
        connection->used = false;
        return;
    case TcpState::kEstablished:
    case TcpState::kSynReceived:
        transmit_segment(*connection, kTcpFlagFin | kTcpFlagAck, nullptr, 0, connection->snd_nxt);
        connection->snd_nxt += 1;
        connection->state = TcpState::kFinWait1;
        if (connection->oldest_sent_ns == 0) {
            connection->oldest_sent_ns = service_now_ns_;
        }
        return;
    case TcpState::kCloseWait:
        transmit_segment(*connection, kTcpFlagFin | kTcpFlagAck, nullptr, 0, connection->snd_nxt);
        connection->snd_nxt += 1;
        connection->state = TcpState::kLastAck;
        if (connection->oldest_sent_ns == 0) {
            connection->oldest_sent_ns = service_now_ns_;
        }
        return;
    default:
        return;
    }
}

void TcpModule::abandon(const TcpTicket& ticket) {
    PendingFire fire{};
    {
        const auto  kGate      = gate_.guard();
        Connection* connection = live_connection(ticket);
        if (connection == nullptr) {
            return;
        }
        if (connection->state == TcpState::kSynSent) {
            transmit_rst(connection->remote_address, connection->remote_port,
                         connection->local_port, connection->snd_nxt, 0, false);
        } else {
            transmit_rst(connection->remote_address, connection->remote_port,
                         connection->local_port, connection->snd_nxt, connection->rcv_nxt, true);
        }
        release_connection(*connection, fire, TcpFarewell::kReset);
    }
    deliver_fire(fire);
}

void TcpModule::deliver_fire(const PendingFire& fire) {
    if (fire.data && fire.events.data != nullptr) {
        fire.events.data(fire.events.context);
    }
    if (fire.accepted && fire.events.accepted != nullptr) {
        fire.events.accepted(fire.events.context);
    }
    if (fire.ended && fire.events.farewell != nullptr) {
        fire.events.farewell(fire.events.context, fire.farewell);
    }
}

void TcpModule::after_ack_movement(Connection& connection, bool moved, unsigned long long now) {
    if (!moved) {
        return;
    }
    connection.in_retransmission = false;
    connection.retransmit_count  = 0;
    if (connection.snd_una == connection.snd_nxt) {
        take_rtt_sample(connection, now);
        disarm_rto_guard(connection);
        connection.oldest_sent_ns = 0;
    }
}

void TcpModule::take_rtt_sample(Connection& connection, unsigned long long now) {
    if (connection.in_retransmission || connection.oldest_sent_ns == 0) {
        return;
    }
    if (now <= connection.oldest_sent_ns) {
        return;
    }
    const unsigned long long kSample = now - connection.oldest_sent_ns;
    if (connection.srtt_ns == 0) {
        connection.srtt_ns   = kSample;
        connection.rttvar_ns = kSample / 2;
    } else {
        const unsigned long long kDiff = connection.srtt_ns > kSample
                                             ? connection.srtt_ns - kSample
                                             : kSample - connection.srtt_ns;
        connection.rttvar_ns           = ((3 * connection.rttvar_ns) + kDiff) / 4;
        connection.srtt_ns             = ((7 * connection.srtt_ns) + kSample) / 8;
    }
    connection.rto_ns = current_rto_of(connection);
}

unsigned long long TcpModule::current_rto_of(const Connection& connection) const {
    if (connection.srtt_ns == 0) {
        return kTcpInitialRtoNanoseconds;
    }
    const unsigned long long kSpread = connection.rttvar_ns * 4 > kTcpRtoGranularityNanoseconds
                                           ? connection.rttvar_ns * 4
                                           : kTcpRtoGranularityNanoseconds;
    unsigned long long       rto     = connection.srtt_ns + kSpread;
    rto                              = std::min(rto, kTcpMaxRtoNanoseconds);
    return rto;
}

void TcpModule::arm_rto_guard(Connection& connection) {
    if (connection.rto_armed) {
        return;
    }
    const unsigned long long kWait =
        connection.rto_ns != 0 ? connection.rto_ns : kTcpInitialRtoNanoseconds;
    const auto kSeat = time::TimerTable::self().arm(&connection, &TcpModule::on_rto_due,
                                                    connection.oldest_sent_ns + kWait);
    if (kSeat.ok()) {
        connection.rto_seat  = kSeat.value();
        connection.rto_armed = true;
    }
}

void TcpModule::disarm_rto_guard(Connection& connection) {
    if (!connection.rto_armed) {
        return;
    }
    time::TimerTable::self().cancel(connection.rto_seat);
    connection.rto_armed = false;
}

void TcpModule::on_rto_due(void* context) {
    auto* connection = static_cast<Connection*>(context);
    if (connection->owner != nullptr) {
        connection->owner->retransmit_due(*connection);
    }
}

void TcpModule::begin_time_wait(Connection& connection) {
    connection.state = TcpState::kTimeWait;
    disarm_rto_guard(connection);
    if (connection.time_wait_armed) {
        return;
    }
    const unsigned long long kWait = 2 * kTcpMslNanoseconds;
    const auto kSeat = time::TimerTable::self().arm(&connection, &TcpModule::on_time_wait_due,
                                                    service_now_ns_ + kWait);
    if (kSeat.ok()) {
        connection.time_wait_seat  = kSeat.value();
        connection.time_wait_armed = true;
    }
}

void TcpModule::on_time_wait_due(void* context) {
    auto* connection = static_cast<Connection*>(context);
    if (connection->owner != nullptr) {
        connection->owner->expire_time_wait(*connection);
    }
}

void TcpModule::expire_time_wait(Connection& connection) {
    connection.time_wait_armed = false;
    if (!connection.used || connection.state != TcpState::kTimeWait) {
        return;
    }
    PendingFire fire{};
    release_connection(connection, fire, TcpFarewell::kGraceful);
    if (service_fire_count_ < kTcpConnectionSeats) {
        service_fires_[service_fire_count_] = fire;
        ++service_fire_count_;
    }
}

void TcpModule::store_out_of_order(Connection& connection, const TcpView& view,
                                   const unsigned char* payload, size_t payload_bytes) {
    if ((view.flags & kTcpFlagFin) != 0 || payload_bytes == 0 || payload_bytes > kTcpMssBytes) {
        ++stats_.out_of_order_dropped;
        return;
    }
    const uint32_t kStart = view.sequence;
    if (!seq_before(kStart, connection.rcv_nxt + static_cast<uint32_t>(kTcpReceiveWindowBytes))) {
        ++stats_.out_of_order_dropped;
        return;
    }
    const uint32_t kEnd = kStart + static_cast<uint32_t>(payload_bytes);
    for (const ReorderSeat& seat : connection.reorder) {
        if (seat.used && seq_before(kStart, seat.start + static_cast<uint32_t>(seat.bytes)) &&
            seq_before(seat.start, kEnd)) {
            ++stats_.out_of_order_dropped;
            return;
        }
    }
    for (ReorderSeat& seat : connection.reorder) {
        if (seat.used) {
            continue;
        }
        seat.used  = true;
        seat.start = kStart;
        seat.bytes = payload_bytes;
        for (size_t index = 0; index < payload_bytes; ++index) {
            seat.data[index] = payload[index];
        }
        ++stats_.out_of_order_stored;
        return;
    }
    ++stats_.out_of_order_dropped;
}

void TcpModule::harvest_reorder(Connection& connection, PendingFire& fire) {
    bool delivered = true;
    while (delivered) {
        delivered = false;
        for (ReorderSeat& seat : connection.reorder) {
            if (!seat.used || seat.start != connection.rcv_nxt) {
                continue;
            }
            const size_t kRoom = kTcpReceiveWindowBytes - connection.inbox.count();
            if (seat.bytes > kRoom) {
                continue;
            }
            for (size_t index = 0; index < seat.bytes; ++index) {
                connection.inbox.push(seat.data[index]);
            }
            connection.rcv_nxt += static_cast<uint32_t>(seat.bytes);
            stats_.bytes_delivered += seat.bytes;
            ++stats_.out_of_order_recovered;
            seat      = ReorderSeat{};
            delivered = true;
            fire.data = true;
        }
    }
}

void TcpModule::resend_oldest(Connection& connection) {
    connection.in_retransmission = true;
    if (connection.state == TcpState::kSynSent) {
        transmit_segment(connection, kTcpFlagSyn, nullptr, 0, connection.snd_una);
        return;
    }
    // FIN follows every queued byte in sequence space. Until those bytes
    // are acknowledged, SND.UNA still names payload, even in a closing state.
    if (connection.outbox.count() == 0 &&
        (connection.state == TcpState::kFinWait1 || connection.state == TcpState::kClosing ||
         connection.state == TcpState::kLastAck)) {
        transmit_segment(connection, kTcpFlagFin | kTcpFlagAck, nullptr, 0, connection.snd_una);
        return;
    }
    const uint32_t kInFlight = connection.snd_nxt - connection.snd_una;
    size_t         stretch   = kInFlight < static_cast<uint32_t>(kTcpMssBytes)
                                   ? static_cast<size_t>(kInFlight)
                                   : static_cast<size_t>(kTcpMssBytes);
    stretch                  = std::min(stretch, static_cast<size_t>(connection.outbox.count()));
    if (stretch == 0) {
        return;
    }
    unsigned char payload[kTcpMssBytes] = {};
    for (size_t index = 0; index < stretch; ++index) {
        unsigned char carried = 0;
        if (!connection.outbox.peek(static_cast<unsigned int>(index), carried)) {
            break;
        }
        payload[index] = carried;
    }
    transmit_segment(connection, kTcpFlagAck | kTcpFlagPsh, payload, stretch, connection.snd_una);
}

void TcpModule::retransmit_due(Connection& connection) {
    connection.rto_armed = false;
    if (!connection.used) {
        return;
    }
    ++stats_.rto_timeouts;
    if (connection.retransmit_count >= kTcpMaxRetransmits) {
        ++stats_.giveups;
        PendingFire fire{};
        release_connection(connection, fire, TcpFarewell::kUnreachable);
        if (service_fire_count_ < kTcpConnectionSeats) {
            service_fires_[service_fire_count_] = fire;
            ++service_fire_count_;
        }
        return;
    }
    ++connection.retransmit_count;
    ++stats_.retransmits_sent;
    resend_oldest(connection);
    unsigned long long wait =
        connection.rto_ns != 0 ? connection.rto_ns : kTcpInitialRtoNanoseconds;
    wait *= 2;
    wait              = std::min(wait, kTcpMaxRtoNanoseconds);
    connection.rto_ns = wait;
    const auto kSeat =
        time::TimerTable::self().arm(&connection, &TcpModule::on_rto_due, service_now_ns_ + wait);
    if (kSeat.ok()) {
        connection.rto_seat  = kSeat.value();
        connection.rto_armed = true;
    }
}

void TcpModule::service(unsigned long long now) {
    PendingFire   fires[kTcpConnectionSeats] = {};
    unsigned long count                      = 0;
    {
        const auto kGate = gate_.guard();
        service_now_ns_  = now;
        time::TimerTable::self().advance(now);
        for (Connection& connection : connections_) {
            if (connection.used && connection.oldest_sent_ns != 0 &&
                connection.snd_una != connection.snd_nxt) {
                arm_rto_guard(connection);
            }
        }
        count = service_fire_count_;
        for (unsigned long index = 0; index < count; ++index) {
            fires[index] = service_fires_[index];
        }
        service_fire_count_ = 0;
    }
    for (unsigned long index = 0; index < count; ++index) {
        deliver_fire(fires[index]);
    }
}

const TcpStats& TcpModule::stats() const {
    return stats_;
}

unsigned long long TcpModule::clock() const {
    return service_now_ns_;
}

uint32_t TcpModule::own_address() const {
    return own_address_;
}

bool TcpModule::holds_bytes(const TcpTicket& ticket) const {
    const auto        kGate      = gate_.guard();
    const Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return true;
    }
    return connection->inbox.count() != 0 || past_fin(connection->state);
}

bool TcpModule::peer_of(const TcpTicket& ticket, uint32_t& address, uint16_t& port) const {
    const auto        kGate      = gate_.guard();
    const Connection* connection = live_connection(ticket);
    if (connection == nullptr) {
        return false;
    }
    address = connection->remote_address;
    port    = connection->remote_port;
    return true;
}

bool TcpModule::alive(const TcpTicket& ticket) const {
    const auto kGate = gate_.guard();
    return live_connection(ticket) != nullptr;
}

}  // namespace cinux::net
