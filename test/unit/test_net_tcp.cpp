#include <stdint.h>

#include <cstddef>

#include "../framework/framework.hpp"
#include "cinux/assert.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/tcp.hpp"
#include "test_assert.hpp"

namespace {

using cinux::base::ReadWord16Be;
using cinux::base::ReadWord32Be;
using cinux::base::WriteWord16Be;
using cinux::base::WriteWord32Be;

void send_to_loopback(void* context, const cinux::net::OutboundDatagram& datagram) {
    auto& device = *static_cast<cinux::driver::net::Loopback*>(context);
    const cinux::driver::net::MacAddress kNobody = {};
    const cinux::driver::net::EtherType  kIpv4{cinux::net::kEtherTypeIpv4};
    cinux::base::safety::Check(device.send_l3(kNobody, kIpv4, datagram.payload, datagram.length),
                               "loopback queue refused a send");
}

void send_via_ipv4(void* context, const cinux::net::OutboundDatagram& datagram) {
    static_cast<cinux::net::Ipv4Module*>(context)->send(datagram);
}

struct Wiring {
    cinux::driver::net::NetPort port;
    cinux::net::NetStack        stack;
    cinux::net::Ipv4Module      ipv4;
    cinux::net::TcpModule       tcp;

    Wiring() {
        cinux::driver::net::Loopback::self().init();
        port.adopt(cinux::driver::net::Loopback::self());
        stack.init();
        stack.attach_port(port);
        const cinux::net::DatagramSink kToWire = {
            .context = &cinux::driver::net::Loopback::self(),
            .send    = send_to_loopback,
        };
        ipv4.init(kToWire, cinux::net::kLoopbackAddress);
        const cinux::net::DatagramSink kViaIpv4 = {.context = &ipv4, .send = send_via_ipv4};
        tcp.init(kViaIpv4, cinux::net::kLoopbackAddress);
        cinux::net::L4Seat tcp_seat;
        tcp_seat.adopt(tcp);
        ipv4.seat_protocol(cinux::net::kIpv4ProtocolTcp, tcp_seat);
        cinux::net::ProtocolSeat ipv4_seat;
        ipv4_seat.adopt(ipv4);
        stack.seat_protocol(cinux::net::kEtherTypeIpv4, ipv4_seat);
    }
};

struct Collector {
    unsigned long data_hits             = 0;
    unsigned long accepted_hits         = 0;
    unsigned long farewells_graceful    = 0;
    unsigned long farewells_reset       = 0;
    unsigned long farewells_unreachable = 0;

    static void on_data(void* context) { ++static_cast<Collector*>(context)->data_hits; }
    static void on_accepted(void* context) { ++static_cast<Collector*>(context)->accepted_hits; }
    static void on_farewell(void* context, cinux::net::TcpFarewell farewell) {
        auto* self = static_cast<Collector*>(context);
        if (farewell == cinux::net::TcpFarewell::kGraceful) {
            ++self->farewells_graceful;
        } else if (farewell == cinux::net::TcpFarewell::kUnreachable) {
            ++self->farewells_unreachable;
        } else {
            ++self->farewells_reset;
        }
    }
};

struct Recorder {
    unsigned char frames[24][cinux::net::kMaxDatagramBytes] = {};
    size_t        lengths[24]                               = {};
    unsigned long count                                     = 0;

    static void take(void* context, const cinux::net::OutboundDatagram& datagram) {
        auto* self = static_cast<Recorder*>(context);
        if (self->count == 24) {
            return;
        }
        for (size_t index = 0; index < datagram.length; ++index) {
            self->frames[self->count][index] = datagram.payload[index];
        }
        self->lengths[self->count] = datagram.length;
        ++self->count;
    }

    [[nodiscard]] uint32_t seq_of(unsigned long which) const {
        return ReadWord32Be(frames[which], 4);
    }
};

void deliver(cinux::net::TcpModule& tcp, const unsigned char* segment, size_t length) {
    const cinux::net::Ipv4View kView = {
        .header_bytes = cinux::net::kMinIpv4HeaderBytes,
        .total_length = static_cast<uint16_t>(cinux::net::kMinIpv4HeaderBytes + length),
        .protocol     = cinux::net::kIpv4ProtocolTcp,
        .source       = cinux::net::kLoopbackAddress,
        .destination  = cinux::net::kLoopbackAddress,
    };
    tcp.handle(segment, length, kView);
}

// NOLINTNEXTLINE(readability-function-size) a segment header names eight facts
void fill_tcp_header(unsigned char* segment, uint16_t sport, uint16_t dport, uint32_t seq,
                     uint32_t ack, unsigned char flags, uint16_t window,
                     const unsigned char* payload, size_t payload_bytes) {
    WriteWord16Be(segment, 0, sport);
    WriteWord16Be(segment, 2, dport);
    WriteWord32Be(segment, 4, seq);
    WriteWord32Be(segment, 8, ack);
    segment[12] = 0x50;
    segment[13] = flags;
    WriteWord16Be(segment, 14, window);
    WriteWord16Be(segment, 16, 0);
    for (size_t index = 0; index < payload_bytes; ++index) {
        segment[cinux::net::kTcpHeaderBytes + index] = payload[index];
    }
    unsigned char pseudo[12] = {};
    WriteWord32Be(pseudo, 0, cinux::net::kLoopbackAddress);
    WriteWord32Be(pseudo, 4, cinux::net::kLoopbackAddress);
    pseudo[8] = 0;
    pseudo[9] = cinux::net::kIpv4ProtocolTcp;
    WriteWord16Be(pseudo, 10, static_cast<uint16_t>(cinux::net::kTcpHeaderBytes + payload_bytes));
    cinux::base::InternetChecksum sum;
    sum.add(pseudo, sizeof(pseudo));
    sum.add(segment, cinux::net::kTcpHeaderBytes + payload_bytes);
    uint16_t checksum = sum.value();
    if (checksum == 0) {
        checksum = 0xFFFF;
    }
    WriteWord16Be(segment, 16, checksum);
}

struct ManualLink {
    bool                        ok          = false;
    uint16_t                    client_port = 0;
    uint32_t                    client_isn  = 0;
    uint32_t                    server_isn  = 0;
    cinux::net::TcpListenTicket listener{};
    cinux::net::TcpTicket       server{};
};

ManualLink drive_handshake(cinux::net::TcpModule& tcp, Recorder& wire, uint16_t port) {
    ManualLink                  link{};
    const cinux::net::TcpEvents kEvents{};
    const auto                  kListener = tcp.listen(port, kEvents);
    if (!kListener.ok()) {
        return link;
    }
    link.listener      = kListener.value();
    const auto kClient = tcp.connect(0, cinux::net::kLoopbackAddress, port, kEvents, 0);
    if (!kClient.ok() || wire.count == 0) {
        return link;
    }
    link.client_isn  = ReadWord32Be(wire.frames[0], 4);
    link.client_port = ReadWord16Be(wire.frames[0], 0);
    deliver(tcp, wire.frames[0], wire.lengths[0]);
    if (wire.count < 2) {
        return link;
    }
    link.server_isn                                = ReadWord32Be(wire.frames[1], 4);
    unsigned char seg[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(seg, link.client_port, port, link.client_isn + 1, link.server_isn + 1,
                    cinux::net::kTcpFlagAck, cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, seg, sizeof(seg));
    const auto kServer = tcp.take_accepted(link.listener);
    if (!kServer.ok()) {
        return link;
    }
    link.server = kServer.value();
    link.ok     = true;
    return link;
}

}  // namespace

TEST("tcp: the word book refuses short and offset-dishonest headers") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    unsigned char short_segment[19] = {};
    deliver(tcp, short_segment, sizeof(short_segment));
    ASSERT_EQ(tcp.stats().refused_short, 1UL);

    unsigned char bad_offset[cinux::net::kTcpHeaderBytes] = {};
    bad_offset[12]                                        = 0x40;
    deliver(tcp, bad_offset, sizeof(bad_offset));
    ASSERT_EQ(tcp.stats().refused_offset, 1UL);
    ASSERT_EQ(tcp.stats().segments_sent, 0UL);
}

TEST("tcp: a checksum that fails the pseudo-header sum is refused by name") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    unsigned char segment[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(segment, 40000, 4242, 1, 0, cinux::net::kTcpFlagAck,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    WriteWord16Be(segment, 16, 0xBEEF);
    deliver(tcp, segment, sizeof(segment));
    ASSERT_EQ(tcp.stats().refused_checksum, 1UL);
    ASSERT_EQ(tcp.stats().unclaimed, 0UL);
    ASSERT_EQ(tcp.stats().resets_sent, 0UL);
}

TEST("tcp: a handshake completes and rings the listener's bell") {
    Wiring     wiring;
    Collector  server_news;
    Collector  client_news;
    const auto kListener = wiring.tcp.listen(4242, {
                                                       .context  = &server_news,
                                                       .data     = &Collector::on_data,
                                                       .accepted = &Collector::on_accepted,
                                                       .farewell = &Collector::on_farewell,
                                                   });
    ASSERT_TRUE(kListener.ok());
    const auto kClient = wiring.tcp.connect(0, cinux::net::kLoopbackAddress, 4242,
                                            {
                                                .context  = &client_news,
                                                .data     = &Collector::on_data,
                                                .accepted = &Collector::on_accepted,
                                                .farewell = &Collector::on_farewell,
                                            },
                                            0);
    ASSERT_TRUE(kClient.ok());

    wiring.stack.poll(16);

    ASSERT_TRUE(wiring.tcp.established(kClient.value()));
    ASSERT_EQ(server_news.accepted_hits, 1UL);
    const auto kServer = wiring.tcp.take_accepted(kListener.value());
    ASSERT_TRUE(kServer.ok());
    ASSERT_TRUE(wiring.tcp.established(kServer.value()));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: bytes travel both ways and the farewell dance completes") {
    Wiring     wiring;
    Collector  server_news;
    Collector  client_news;
    const auto kListener = wiring.tcp.listen(4243, {
                                                       .context  = &server_news,
                                                       .data     = &Collector::on_data,
                                                       .accepted = &Collector::on_accepted,
                                                       .farewell = &Collector::on_farewell,
                                                   });
    const auto kClient   = wiring.tcp.connect(0, cinux::net::kLoopbackAddress, 4243,
                                              {
                                                  .context  = &client_news,
                                                  .data     = &Collector::on_data,
                                                  .accepted = &Collector::on_accepted,
                                                  .farewell = &Collector::on_farewell,
                                              },
                                              0);
    wiring.stack.poll(16);
    const auto kServer = wiring.tcp.take_accepted(kListener.value());
    ASSERT_TRUE(kServer.ok());

    const unsigned char kPing[4] = {'p', 'i', 'n', 'g'};
    const auto          kGiven   = wiring.tcp.send(kClient.value(), kPing, sizeof(kPing), 0);
    ASSERT_TRUE(kGiven.ok());
    ASSERT_EQ(kGiven.value(), sizeof(kPing));
    wiring.stack.poll(16);
    ASSERT_EQ(server_news.data_hits, 1UL);
    unsigned char inbox[16] = {};
    const auto    kTaken    = wiring.tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_TRUE(kTaken.ok());
    ASSERT_EQ(kTaken.value(), sizeof(kPing));
    ASSERT_EQ(inbox[0], static_cast<unsigned char>('p'));
    ASSERT_EQ(inbox[3], static_cast<unsigned char>('g'));

    const unsigned char kPong[4] = {'p', 'o', 'n', 'g'};
    const auto          kBack    = wiring.tcp.send(kServer.value(), kPong, sizeof(kPong), 0);
    ASSERT_TRUE(kBack.ok());
    wiring.stack.poll(16);
    ASSERT_EQ(client_news.data_hits, 1UL);
    const auto kRound = wiring.tcp.receive(kClient.value(), inbox, sizeof(inbox));
    ASSERT_TRUE(kRound.ok());
    ASSERT_EQ(kRound.value(), sizeof(kPong));
    ASSERT_EQ(inbox[1], static_cast<unsigned char>('o'));

    wiring.tcp.close(kClient.value());
    wiring.stack.poll(16);
    const auto kEof = wiring.tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_TRUE(kEof.ok());
    ASSERT_EQ(kEof.value(), 0UL);

    wiring.tcp.close(kServer.value());
    wiring.stack.poll(16);
    wiring.tcp.service((2 * cinux::net::kTcpMslNanoseconds) + 1);
    wiring.stack.poll(16);
    ASSERT_EQ(client_news.farewells_graceful, 1UL);
    ASSERT_EQ(server_news.farewells_graceful, 1UL);
}

TEST("tcp: an open against a closed port is answered with a reset") {
    Wiring     wiring;
    Collector  client_news;
    const auto kClient = wiring.tcp.connect(0, cinux::net::kLoopbackAddress, 4999,
                                            {
                                                .context  = &client_news,
                                                .data     = &Collector::on_data,
                                                .accepted = &Collector::on_accepted,
                                                .farewell = &Collector::on_farewell,
                                            },
                                            0);
    ASSERT_TRUE(kClient.ok());

    wiring.stack.poll(16);

    ASSERT_FALSE(wiring.tcp.established(kClient.value()));
    ASSERT_EQ(client_news.farewells_reset, 1UL);
    ASSERT_EQ(wiring.tcp.stats().resets_sent, 1UL);
    ASSERT_EQ(wiring.tcp.stats().resets_received, 1UL);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: stale and out-of-order segments get named answers") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4301);
    ASSERT_TRUE(kLink.ok);
    ASSERT_TRUE(tcp.established(kLink.server));

    const unsigned char kAb[2]                                         = {'a', 'b'};
    unsigned char       seg[cinux::net::kTcpHeaderBytes + sizeof(kAb)] = {};
    fill_tcp_header(seg, kLink.client_port, 4301, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kAb, sizeof(kAb));
    deliver(tcp, seg, sizeof(seg));
    unsigned char inbox[8] = {};
    const auto    kTaken   = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_TRUE(kTaken.ok());
    ASSERT_EQ(kTaken.value(), sizeof(kAb));

    unsigned char gap[cinux::net::kTcpHeaderBytes + sizeof(kAb)] = {};
    fill_tcp_header(gap, kLink.client_port, 4301, kLink.client_isn + 5, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kAb, sizeof(kAb));
    deliver(tcp, gap, sizeof(gap));
    ASSERT_EQ(tcp.stats().out_of_order_stored, 1UL);

    unsigned char overlap[cinux::net::kTcpHeaderBytes + 1] = {};
    fill_tcp_header(overlap, kLink.client_port, 4301, kLink.client_isn + 5, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kAb, 1);
    deliver(tcp, overlap, sizeof(overlap));
    ASSERT_EQ(tcp.stats().out_of_order_dropped, 1UL);

    deliver(tcp, seg, sizeof(seg));
    ASSERT_EQ(tcp.stats().stale_dropped, 1UL);

    const auto kAgain = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_FALSE(kAgain.ok());
    ASSERT_TRUE(kAgain.error() == cinux::base::KernelError::kWouldBlock);
}

TEST("tcp: an acknowledgement naming unsent bytes is dropped") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4302);
    ASSERT_TRUE(kLink.ok);
    const unsigned long kFrames = wire.count;

    unsigned char seg[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(seg, kLink.client_port, 4302, kLink.client_isn + 1, kLink.server_isn + 100,
                    cinux::net::kTcpFlagAck, cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, seg, sizeof(seg));

    ASSERT_EQ(tcp.stats().ack_beyond_dropped, 1UL);
    ASSERT_EQ(wire.count, kFrames);
}

TEST("tcp: the connection table refuses a ninth connection") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const cinux::net::TcpEvents kEvents{};
    for (unsigned long attempt = 0; attempt < cinux::net::kTcpConnectionSeats; ++attempt) {
        const auto kSeat = tcp.connect(0, cinux::net::kLoopbackAddress,
                                       static_cast<uint16_t>(5000 + attempt), kEvents, 0);
        ASSERT_TRUE(kSeat.ok());
    }
    const auto kNinth = tcp.connect(0, cinux::net::kLoopbackAddress, 6000, kEvents, 0);
    ASSERT_FALSE(kNinth.ok());
}

TEST("tcp: ephemeral ports rise and a closed syn-sent seat is reused") {
    cinux::net::TcpModule       tcp;
    Recorder                    wire;
    const cinux::net::TcpEvents kEvents{};
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const auto kFirst = tcp.connect(0, cinux::net::kLoopbackAddress, 5001, kEvents, 0);
    ASSERT_TRUE(kFirst.ok());
    const auto kSecond = tcp.connect(0, cinux::net::kLoopbackAddress, 5002, kEvents, 0);
    ASSERT_TRUE(kSecond.ok());
    ASSERT_TRUE(wire.count >= 2);
    const auto kFirstPort  = ReadWord16Be(wire.frames[0], 0);
    const auto kSecondPort = ReadWord16Be(wire.frames[1], 0);
    ASSERT_NE(kFirstPort, kSecondPort);
    ASSERT_TRUE(kFirstPort >= cinux::net::kEphemeralPortFirst);

    tcp.close(kFirst.value());
    const auto kThird = tcp.connect(0, cinux::net::kLoopbackAddress, 5003, kEvents, 0);
    ASSERT_TRUE(kThird.ok());
    ASSERT_EQ(kThird.value().slot, kFirst.value().slot);
    ASSERT_EQ(kThird.value().stamp, kFirst.value().stamp + 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: silence re-sends the oldest segment and an acknowledgement silences it") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4305);
    ASSERT_TRUE(kLink.ok);

    const unsigned char      kPing[4] = {'p', 'i', 'n', 'g'};
    const unsigned long long kBase    = cinux::net::kTcpInitialRtoNanoseconds;
    const auto               kGiven   = tcp.send(kLink.server, kPing, sizeof(kPing), kBase);
    ASSERT_TRUE(kGiven.ok());
    const unsigned long kAfterSend = wire.count;

    tcp.service(kBase);
    ASSERT_EQ(wire.count, kAfterSend);

    tcp.service(kBase + cinux::net::kTcpInitialRtoNanoseconds + 1);
    ASSERT_EQ(tcp.stats().rto_timeouts, 1UL);
    ASSERT_EQ(tcp.stats().retransmits_sent, 1UL);
    ASSERT_EQ(wire.count, kAfterSend + 1);
    ASSERT_EQ(wire.seq_of(wire.count - 1), kLink.server_isn + 1);
    ASSERT_EQ(wire.frames[wire.count - 1][cinux::net::kTcpHeaderBytes],
              static_cast<unsigned char>('p'));

    const unsigned long long kAckTime = kBase + (2 * cinux::net::kTcpInitialRtoNanoseconds);
    tcp.service(kAckTime);
    unsigned char ack[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(ack, kLink.client_port, 4305, kLink.client_isn + 1,
                    kLink.server_isn + 1 + sizeof(kPing), cinux::net::kTcpFlagAck,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, ack, sizeof(ack));

    tcp.service(kAckTime + (8 * cinux::net::kTcpMaxRtoNanoseconds));
    ASSERT_EQ(tcp.stats().retransmits_sent, 1UL);
    ASSERT_EQ(wire.count, kAfterSend + 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: close retransmits unacknowledged payload before its FIN") {
    using namespace cinux;
    net::TcpModule tcp;
    Recorder       wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, net::kLoopbackAddress);
    const ManualLink kLink = drive_handshake(tcp, wire, 4320);
    ASSERT_TRUE(kLink.ok);
    const auto          kBase = net::kTcpInitialRtoNanoseconds;
    const unsigned char kHi[] = {'h', 'i', '\n'};
    ASSERT_TRUE(tcp.send(kLink.server, kHi, sizeof(kHi), kBase).ok());
    tcp.close(kLink.server);
    tcp.service(kBase);
    tcp.service(kBase + net::kTcpInitialRtoNanoseconds + 1);
    ASSERT_EQ(wire.seq_of(wire.count - 1), kLink.server_isn + 1);
    ASSERT_EQ(wire.lengths[wire.count - 1], net::kTcpHeaderBytes + sizeof(kHi));
    ASSERT_EQ(wire.frames[wire.count - 1][13] & net::kTcpFlagFin, 0U);
    ASSERT_EQ(wire.frames[wire.count - 1][net::kTcpHeaderBytes], static_cast<unsigned char>('h'));
    tcp.abandon(kLink.server);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: a silent peer exhausts the retries and the connection is given up") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    Collector             news;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const unsigned long long kBase   = cinux::net::kTcpInitialRtoNanoseconds;
    const auto               kClient = tcp.connect(0, cinux::net::kLoopbackAddress, 4306,
                                                   {
                                                       .context  = &news,
                                                       .data     = &Collector::on_data,
                                                       .accepted = &Collector::on_accepted,
                                                       .farewell = &Collector::on_farewell,
                                                   },
                                                   kBase);
    ASSERT_TRUE(kClient.ok());
    const unsigned long kAfterSyn = wire.count;

    tcp.service(kBase);
    unsigned long long wait  = cinux::net::kTcpInitialRtoNanoseconds;
    unsigned long long clock = kBase;
    for (unsigned pass = 0; pass < 5; ++pass) {
        clock += wait + 1;
        tcp.service(clock);
        wait *= 2;
        ASSERT_EQ(tcp.stats().retransmits_sent, pass + 1UL);
    }
    ASSERT_EQ(wire.count, kAfterSyn + 5);

    clock += wait + 1;
    tcp.service(clock);
    ASSERT_EQ(tcp.stats().rto_timeouts, 6UL);
    ASSERT_EQ(tcp.stats().giveups, 1UL);
    ASSERT_EQ(tcp.stats().retransmits_sent, 5UL);
    ASSERT_EQ(news.farewells_unreachable, 1UL);
    ASSERT_EQ(wire.count, kAfterSyn + 5);

    const unsigned char kOne[1] = {'x'};
    const auto          kDead   = tcp.send(kClient.value(), kOne, sizeof(kOne), clock);
    ASSERT_FALSE(kDead.ok());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: a filled gap releases the parked segment in order") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4307);
    ASSERT_TRUE(kLink.ok);

    const unsigned char kLate[2]                                          = {'c', 'd'};
    unsigned char       late[cinux::net::kTcpHeaderBytes + sizeof(kLate)] = {};
    fill_tcp_header(late, kLink.client_port, 4307, kLink.client_isn + 3, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kLate, sizeof(kLate));
    deliver(tcp, late, sizeof(late));
    ASSERT_EQ(tcp.stats().out_of_order_stored, 1UL);
    const auto kEmpty = tcp.receive(kLink.server, late, sizeof(late));
    ASSERT_FALSE(kEmpty.ok());
    ASSERT_TRUE(kEmpty.error() == cinux::base::KernelError::kWouldBlock);

    const unsigned char kMissing[2]                                             = {'a', 'b'};
    unsigned char       missing[cinux::net::kTcpHeaderBytes + sizeof(kMissing)] = {};
    fill_tcp_header(missing, kLink.client_port, 4307, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kMissing, sizeof(kMissing));
    deliver(tcp, missing, sizeof(missing));
    ASSERT_EQ(tcp.stats().out_of_order_recovered, 1UL);

    unsigned char inbox[8] = {};
    const auto    kTaken   = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_TRUE(kTaken.ok());
    ASSERT_EQ(kTaken.value(), 4UL);
    ASSERT_EQ(inbox[0], static_cast<unsigned char>('a'));
    ASSERT_EQ(inbox[3], static_cast<unsigned char>('d'));
}

TEST("tcp: three duplicate acknowledgements re-send the oldest segment at once") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4308);
    ASSERT_TRUE(kLink.ok);

    const unsigned char      kPing[4] = {'p', 'i', 'n', 'g'};
    const unsigned long long kBase    = cinux::net::kTcpInitialRtoNanoseconds;
    const auto               kGiven   = tcp.send(kLink.server, kPing, sizeof(kPing), kBase);
    ASSERT_TRUE(kGiven.ok());
    const unsigned long kAfterSend = wire.count;

    unsigned char dup[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(dup, kLink.client_port, 4308, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck, cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, dup, sizeof(dup));
    deliver(tcp, dup, sizeof(dup));
    ASSERT_EQ(tcp.stats().fast_retransmits, 0UL);
    deliver(tcp, dup, sizeof(dup));
    ASSERT_EQ(tcp.stats().fast_retransmits, 1UL);
    ASSERT_EQ(tcp.stats().retransmits_sent, 0UL);
    ASSERT_EQ(wire.count, kAfterSend + 1);
    ASSERT_EQ(wire.seq_of(wire.count - 1), kLink.server_isn + 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("tcp: the peer's advertised window gates sending and reopens on drain") {
    Wiring     wiring;
    Collector  server_news;
    Collector  client_news;
    const auto kListener = wiring.tcp.listen(4310, {
                                                       .context  = &server_news,
                                                       .data     = &Collector::on_data,
                                                       .accepted = &Collector::on_accepted,
                                                       .farewell = &Collector::on_farewell,
                                                   });
    const auto kClient   = wiring.tcp.connect(0, cinux::net::kLoopbackAddress, 4310,
                                              {
                                                  .context  = &client_news,
                                                  .data     = &Collector::on_data,
                                                  .accepted = &Collector::on_accepted,
                                                  .farewell = &Collector::on_farewell,
                                              },
                                              0);
    wiring.stack.poll(16);
    const auto kServer = wiring.tcp.take_accepted(kListener.value());
    ASSERT_TRUE(kServer.ok());

    unsigned char flood[cinux::net::kTcpSendWindowBytes + 64] = {};
    const auto    kFirst = wiring.tcp.send(kClient.value(), flood, sizeof(flood), 0);
    ASSERT_TRUE(kFirst.ok());
    ASSERT_EQ(kFirst.value(), cinux::net::kTcpSendWindowBytes);
    wiring.stack.poll(16);
    ASSERT_EQ(server_news.data_hits, 3UL);

    const auto kShut = wiring.tcp.send(kClient.value(), flood, 8, 0);
    ASSERT_TRUE(kShut.ok());
    ASSERT_EQ(kShut.value(), 0UL);

    unsigned char drain[32] = {};
    size_t        seen      = 0;
    while (seen < cinux::net::kTcpSendWindowBytes) {
        const auto kTaken = wiring.tcp.receive(kServer.value(), drain, sizeof(drain));
        ASSERT_TRUE(kTaken.ok());
        seen += kTaken.value();
    }
    wiring.stack.poll(16);

    const unsigned char kMore[4] = {'m', 'o', 'r', 'e'};
    const auto          kAgain   = wiring.tcp.send(kClient.value(), kMore, sizeof(kMore), 0);
    ASSERT_TRUE(kAgain.ok());
    ASSERT_EQ(kAgain.value(), sizeof(kMore));
}

TEST("tcp: the closing side lingers two maximum segment lifetimes") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4311);
    ASSERT_TRUE(kLink.ok);
    const unsigned long long kClock = 100;
    tcp.service(kClock);

    tcp.close(kLink.server);
    unsigned char ack[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(ack, kLink.client_port, 4311, kLink.client_isn + 1, kLink.server_isn + 2,
                    cinux::net::kTcpFlagAck, cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, ack, sizeof(ack));
    unsigned char fin[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(fin, kLink.client_port, 4311, kLink.client_isn + 1, kLink.server_isn + 2,
                    cinux::net::kTcpFlagFin | cinux::net::kTcpFlagAck,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, fin, sizeof(fin));

    const unsigned char kOne[1]    = {'x'};
    const auto          kLingering = tcp.send(kLink.server, kOne, sizeof(kOne), kClock);
    ASSERT_TRUE(kLingering.error() == cinux::base::KernelError::kBrokenPipe);
    const unsigned long kFrames = wire.count;
    deliver(tcp, fin, sizeof(fin));
    ASSERT_EQ(wire.count, kFrames + 1);

    tcp.service(kClock + (2 * cinux::net::kTcpMslNanoseconds) + 1);
    const auto kGone = tcp.send(kLink.server, kOne, sizeof(kOne), kClock);
    ASSERT_TRUE(kGone.error() == cinux::base::KernelError::kNotFound);
}

TEST("tcp: a fresh SYN during the linger takes the seat at once") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4312);
    ASSERT_TRUE(kLink.ok);
    const unsigned long long kClock = 100;
    tcp.service(kClock);

    tcp.close(kLink.server);
    unsigned char ack[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(ack, kLink.client_port, 4312, kLink.client_isn + 1, kLink.server_isn + 2,
                    cinux::net::kTcpFlagAck, cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, ack, sizeof(ack));
    unsigned char fin[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(fin, kLink.client_port, 4312, kLink.client_isn + 1, kLink.server_isn + 2,
                    cinux::net::kTcpFlagFin | cinux::net::kTcpFlagAck,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, fin, sizeof(fin));

    const unsigned long kFrames                            = wire.count;
    unsigned char       fresh[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(fresh, kLink.client_port, 4312, 9000, 0, cinux::net::kTcpFlagSyn,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, fresh, sizeof(fresh));
    ASSERT_EQ(wire.count, kFrames + 1);
    ASSERT_NE(wire.seq_of(wire.count - 1), kLink.server_isn);
    ASSERT_EQ(tcp.stats().unclaimed, 0UL);

    const unsigned char kOne[1] = {'x'};
    const auto          kGone   = tcp.send(kLink.server, kOne, sizeof(kOne), kClock);
    ASSERT_TRUE(kGone.error() == cinux::base::KernelError::kNotFound);
}

TEST("tcp: an out-of-order storm fills the repair seats and drops by name") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4313);
    ASSERT_TRUE(kLink.ok);

    const unsigned char kTwo[2] = {'x', 'y'};
    for (unsigned long wave = 0; wave < cinux::net::kTcpReorderSeats; ++wave) {
        unsigned char seg[cinux::net::kTcpHeaderBytes + sizeof(kTwo)] = {};
        fill_tcp_header(seg, kLink.client_port, 4313,
                        kLink.client_isn + 1 + static_cast<uint32_t>((wave + 1) * sizeof(kTwo)),
                        kLink.server_isn + 1, cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                        cinux::net::kTcpReceiveWindowBytes, kTwo, sizeof(kTwo));
        deliver(tcp, seg, sizeof(seg));
    }
    ASSERT_EQ(tcp.stats().out_of_order_stored, cinux::net::kTcpReorderSeats);

    unsigned char extra[cinux::net::kTcpHeaderBytes + sizeof(kTwo)] = {};
    fill_tcp_header(
        extra, kLink.client_port, 4313,
        kLink.client_isn + 1 + static_cast<uint32_t>((cinux::net::kTcpReorderSeats + 1) * 2),
        kLink.server_isn + 1, cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
        cinux::net::kTcpReceiveWindowBytes, kTwo, sizeof(kTwo));
    deliver(tcp, extra, sizeof(extra));
    ASSERT_EQ(tcp.stats().out_of_order_dropped, 1UL);

    unsigned char gap[cinux::net::kTcpHeaderBytes + sizeof(kTwo)] = {};
    fill_tcp_header(gap, kLink.client_port, 4313, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kTwo, sizeof(kTwo));
    deliver(tcp, gap, sizeof(gap));
    ASSERT_EQ(tcp.stats().out_of_order_recovered, cinux::net::kTcpReorderSeats);

    unsigned char inbox[32] = {};
    const size_t  kWant     = (cinux::net::kTcpReorderSeats + 1) * 2;
    const auto    kTaken    = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_EQ(kTaken.value(), kWant);
}

TEST("tcp: a stale storm never moves the read cursor") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4314);
    ASSERT_TRUE(kLink.ok);

    const unsigned char kTwo[2]                                         = {'o', 'l'};
    unsigned char       seg[cinux::net::kTcpHeaderBytes + sizeof(kTwo)] = {};
    fill_tcp_header(seg, kLink.client_port, 4314, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
                    cinux::net::kTcpReceiveWindowBytes, kTwo, sizeof(kTwo));
    deliver(tcp, seg, sizeof(seg));
    for (unsigned long wave = 0; wave < 4; ++wave) {
        deliver(tcp, seg, sizeof(seg));
    }
    ASSERT_EQ(tcp.stats().stale_dropped, 4UL);

    unsigned char inbox[8] = {};
    const auto    kTaken   = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_EQ(kTaken.value(), sizeof(kTwo));
    const auto kDry = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_TRUE(kDry.error() == cinux::base::KernelError::kWouldBlock);
}

TEST("tcp: a reset on a live connection releases the seat at once") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4315);
    ASSERT_TRUE(kLink.ok);

    unsigned char rst[cinux::net::kTcpHeaderBytes] = {};
    fill_tcp_header(rst, kLink.client_port, 4315, kLink.client_isn + 1, kLink.server_isn + 1,
                    cinux::net::kTcpFlagRst | cinux::net::kTcpFlagAck,
                    cinux::net::kTcpReceiveWindowBytes, nullptr, 0);
    deliver(tcp, rst, sizeof(rst));
    ASSERT_EQ(tcp.stats().resets_received, 1UL);
    ASSERT_FALSE(tcp.established(kLink.server));
    const unsigned char kOne[1] = {'x'};
    const auto          kGone   = tcp.send(kLink.server, kOne, sizeof(kOne), 0);
    ASSERT_TRUE(kGone.error() == cinux::base::KernelError::kNotFound);
}

TEST("tcp: a segment beyond the receive window is refused without a seat") {
    cinux::net::TcpModule tcp;
    Recorder              wire;
    tcp.init({.context = &wire, .send = &Recorder::take}, cinux::net::kLoopbackAddress);

    const ManualLink kLink = drive_handshake(tcp, wire, 4316);
    ASSERT_TRUE(kLink.ok);

    const unsigned char kTwo[2]                                             = {'f', 'g'};
    unsigned char       far_seg[cinux::net::kTcpHeaderBytes + sizeof(kTwo)] = {};
    fill_tcp_header(
        far_seg, kLink.client_port, 4316,
        kLink.client_isn + 1 + static_cast<uint32_t>(cinux::net::kTcpReceiveWindowBytes),
        kLink.server_isn + 1, cinux::net::kTcpFlagAck | cinux::net::kTcpFlagPsh,
        cinux::net::kTcpReceiveWindowBytes, kTwo, sizeof(kTwo));
    deliver(tcp, far_seg, sizeof(far_seg));
    ASSERT_EQ(tcp.stats().out_of_order_dropped, 1UL);
    ASSERT_EQ(tcp.stats().out_of_order_stored, 0UL);

    unsigned char inbox[8] = {};
    const auto    kDry     = tcp.receive(kLink.server, inbox, sizeof(inbox));
    ASSERT_TRUE(kDry.error() == cinux::base::KernelError::kWouldBlock);
}

TEST("tcp: a full loopback story survives four pump passes") {
    Wiring     wiring;
    Collector  server_news;
    Collector  client_news;
    const auto kListener = wiring.tcp.listen(4317, {
                                                       .context  = &server_news,
                                                       .data     = &Collector::on_data,
                                                       .accepted = &Collector::on_accepted,
                                                       .farewell = &Collector::on_farewell,
                                                   });
    const auto kClient   = wiring.tcp.connect(0, cinux::net::kLoopbackAddress, 4317,
                                              {
                                                  .context  = &client_news,
                                                  .data     = &Collector::on_data,
                                                  .accepted = &Collector::on_accepted,
                                                  .farewell = &Collector::on_farewell,
                                              },
                                              0);
    wiring.stack.poll(4);
    ASSERT_TRUE(wiring.tcp.established(kClient.value()));
    const auto kServer = wiring.tcp.take_accepted(kListener.value());
    ASSERT_TRUE(kServer.ok());

    const unsigned char kPing[4] = {'p', 'i', 'n', 'g'};
    const auto          kGiven   = wiring.tcp.send(kClient.value(), kPing, sizeof(kPing), 0);
    ASSERT_EQ(kGiven.value(), sizeof(kPing));
    wiring.stack.poll(4);
    unsigned char inbox[8] = {};
    const auto    kArrived = wiring.tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_EQ(kArrived.value(), sizeof(kPing));

    const unsigned char kPong[4] = {'p', 'o', 'n', 'g'};
    const auto          kBack    = wiring.tcp.send(kServer.value(), kPong, sizeof(kPong), 0);
    ASSERT_EQ(kBack.value(), sizeof(kPong));
    wiring.stack.poll(4);
    const auto kReturned = wiring.tcp.receive(kClient.value(), inbox, sizeof(inbox));
    ASSERT_EQ(kReturned.value(), sizeof(kPong));

    wiring.tcp.close(kClient.value());
    wiring.stack.poll(4);
    const auto kEof = wiring.tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_EQ(kEof.value(), 0UL);
    wiring.tcp.close(kServer.value());
    wiring.stack.poll(4);
    wiring.tcp.service((2 * cinux::net::kTcpMslNanoseconds) + 1);
    wiring.stack.poll(4);
    ASSERT_EQ(client_news.farewells_graceful + server_news.farewells_graceful, 2UL);
}

int main() {
    return cinux::test::RunAll();
}
