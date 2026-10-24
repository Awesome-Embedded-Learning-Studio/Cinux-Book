#include <stdint.h>

#include <cstddef>

#include "../framework/framework.hpp"
#include "cinux/assert.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/udp.hpp"
#include "test_assert.hpp"

namespace {

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
    cinux::net::UdpModule       udp;

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
        udp.init(kViaIpv4, cinux::net::kLoopbackAddress);
        cinux::net::L4Seat udp_seat;
        udp_seat.adopt(udp);
        ipv4.seat_protocol(cinux::net::kIpv4ProtocolUdp, udp_seat);
        cinux::net::ProtocolSeat ipv4_seat;
        ipv4_seat.adopt(ipv4);
        stack.seat_protocol(cinux::net::kEtherTypeIpv4, ipv4_seat);
    }
};

struct Collector {
    cinux::net::UdpDelivery last     = {};
    unsigned char           copy[64] = {};
    unsigned long           hits     = 0;

    static void take(void* context, const cinux::net::UdpDelivery& delivery) {
        auto* self = static_cast<Collector*>(context);
        self->last = delivery;
        for (size_t index = 0; index < delivery.length && index < sizeof(self->copy); ++index) {
            self->copy[index] = delivery.bytes[index];
        }
        ++self->hits;
    }
};

void fill_udp_header(unsigned char* datagram, uint16_t source, uint16_t destination,
                     uint16_t checksum) {
    cinux::base::WriteWord16Be(datagram, 0, source);
    cinux::base::WriteWord16Be(datagram, 2, destination);
    cinux::base::WriteWord16Be(datagram, 4, cinux::net::kUdpHeaderBytes);
    cinux::base::WriteWord16Be(datagram, 6, checksum);
}

void fill_ipv4_over_udp(unsigned char* datagram, size_t udp_bytes) {
    const auto kTotal = cinux::net::kMinIpv4HeaderBytes + udp_bytes;
    datagram[0]       = 0x45;
    datagram[1]       = 0;
    cinux::base::WriteWord16Be(datagram, 2, static_cast<uint16_t>(kTotal));
    datagram[4] = 0;
    datagram[5] = 0;
    datagram[6] = 0;
    datagram[7] = 0;
    datagram[8] = cinux::net::kIpv4TimeToLive;
    datagram[9] = cinux::net::kIpv4ProtocolUdp;
    cinux::base::WriteWord16Be(datagram, 10, 0);
    cinux::base::WriteWord32Be(datagram, 12, cinux::net::kLoopbackAddress);
    cinux::base::WriteWord32Be(datagram, 16, cinux::net::kLoopbackAddress);
    cinux::base::InternetChecksum header;
    header.add(datagram, cinux::net::kMinIpv4HeaderBytes);
    cinux::base::WriteWord16Be(datagram, 10, header.value());
}

uint16_t pseudo_udp_checksum(uint32_t source, uint32_t destination, const unsigned char* datagram,
                             uint16_t udp_bytes) {
    unsigned char pseudo[12] = {};
    cinux::base::WriteWord32Be(pseudo, 0, source);
    cinux::base::WriteWord32Be(pseudo, 4, destination);
    pseudo[8] = 0;
    pseudo[9] = cinux::net::kIpv4ProtocolUdp;
    cinux::base::WriteWord16Be(pseudo, 10, udp_bytes);
    cinux::base::InternetChecksum sum;
    sum.add(pseudo, sizeof(pseudo));
    sum.add(datagram, udp_bytes);
    return sum.value();
}

}  // namespace

TEST("udp: the word book refuses a header shorter than eight bytes") {
    Wiring        wiring;
    unsigned char frame[32] = {};
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes - 1);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes - 1);
    ASSERT_EQ(wiring.udp.stats().refused_short, 1UL);
    ASSERT_EQ(wiring.udp.stats().delivered, 0UL);
}

TEST("udp: a length that argues with the bytes it arrived in is refused") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    unsigned char frame[40] = {};
    fill_udp_header(frame + cinux::net::kMinIpv4HeaderBytes, 40000, 5353, 0);
    cinux::base::WriteWord16Be(frame + cinux::net::kMinIpv4HeaderBytes, 4,
                               cinux::net::kUdpHeaderBytes + 4);
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(wiring.udp.stats().refused_length, 1UL);
    ASSERT_EQ(sink.hits, 0UL);
}

TEST("udp: a checksum that does not fold to zero dies counted") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    unsigned char  frame[40] = {};
    unsigned char* udp       = frame + cinux::net::kMinIpv4HeaderBytes;
    fill_udp_header(udp, 40000, 5353, 0);
    cinux::base::WriteWord16Be(udp, 6, 0x1234);
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(wiring.udp.stats().refused_checksum, 1UL);
    ASSERT_EQ(sink.hits, 0UL);
}

TEST("udp: an honest zero checksum means none was computed and passes") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    unsigned char  frame[40] = {};
    unsigned char* udp       = frame + cinux::net::kMinIpv4HeaderBytes;
    fill_udp_header(udp, 40000, 5353, 0);
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(wiring.udp.stats().delivered, 1UL);
    ASSERT_EQ(sink.hits, 1UL);
    ASSERT_EQ(sink.last.sender_port, 40000U);
}

TEST("udp: a bound listener receives the payload and the return address") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    unsigned char  frame[48] = {};
    unsigned char* udp       = frame + cinux::net::kMinIpv4HeaderBytes;
    fill_udp_header(udp, 40000, 5353, 0);
    udp[8]                = 'd';
    udp[9]                = 'n';
    udp[10]               = 's';
    const uint16_t kBytes = cinux::net::kUdpHeaderBytes + 3;
    cinux::base::WriteWord16Be(udp, 4, kBytes);
    cinux::base::WriteWord16Be(udp, 6,
                               pseudo_udp_checksum(cinux::net::kLoopbackAddress,
                                                   cinux::net::kLoopbackAddress, udp, kBytes));
    fill_ipv4_over_udp(frame, kBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + kBytes);

    ASSERT_EQ(wiring.udp.stats().delivered, 1UL);
    ASSERT_EQ(sink.hits, 1UL);
    ASSERT_EQ(sink.last.length, 3UL);
    ASSERT_EQ(sink.copy[0], static_cast<unsigned char>('d'));
    ASSERT_EQ(sink.copy[1], static_cast<unsigned char>('n'));
    ASSERT_EQ(sink.copy[2], static_cast<unsigned char>('s'));
    ASSERT_EQ(sink.last.sender_port, 40000U);
    ASSERT_EQ(sink.last.sender, cinux::net::kLoopbackAddress);
}

TEST("udp: a datagram nobody claims dies counted, not loudly") {
    Wiring         wiring;
    unsigned char  frame[40] = {};
    unsigned char* udp       = frame + cinux::net::kMinIpv4HeaderBytes;
    fill_udp_header(udp, 40000, 9999, 0);
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(wiring.udp.stats().unclaimed, 1UL);
}

TEST("udp: an unbound port stops hearing") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    unsigned char  frame[40] = {};
    unsigned char* udp       = frame + cinux::net::kMinIpv4HeaderBytes;
    fill_udp_header(udp, 40000, 5353, 0);
    fill_ipv4_over_udp(frame, cinux::net::kUdpHeaderBytes);
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(sink.hits, 1UL);

    ASSERT_TRUE(wiring.udp.unbind(5353));
    wiring.ipv4.handle(frame, cinux::net::kMinIpv4HeaderBytes + cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(sink.hits, 1UL);
    ASSERT_EQ(wiring.udp.stats().unclaimed, 1UL);
}

TEST("udp: a taken port refuses a second listener until freed") {
    Wiring    wiring;
    Collector first;
    Collector second;
    ASSERT_TRUE(wiring.udp.bind(5353, &first, &Collector::take));
    ASSERT_FALSE(wiring.udp.bind(5353, &second, &Collector::take));
    ASSERT_FALSE(wiring.udp.bind(5353, &first, nullptr));
    ASSERT_FALSE(wiring.udp.unbind(9999));
    ASSERT_TRUE(wiring.udp.unbind(5353));
    ASSERT_TRUE(wiring.udp.bind(5353, &second, &Collector::take));
}

TEST("udp: the ephemeral range answers free ports and skips taken ones") {
    Wiring         wiring;
    Collector      sink;
    const uint16_t kFirst = wiring.udp.ephemeral_port();
    ASSERT_EQ(kFirst, cinux::net::kUdpEphemeralFirst);
    ASSERT_TRUE(wiring.udp.bind(kFirst, &sink, &Collector::take));
    const uint16_t kNext = wiring.udp.ephemeral_port();
    ASSERT_EQ(kNext, static_cast<uint16_t>(cinux::net::kUdpEphemeralFirst + 1));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("udp: a round trip over the loopback delivers the payload unchanged") {
    Wiring    wiring;
    Collector sink;
    wiring.udp.bind(5353, &sink, &Collector::take);

    const unsigned char kAsk[3] = {7, 8, 9};
    ASSERT_TRUE(wiring.udp.send(cinux::net::kLoopbackAddress, 40000, 5353, kAsk, sizeof(kAsk)));
    ASSERT_EQ(wiring.udp.stats().sent, 1UL);
    wiring.stack.poll(4);

    ASSERT_EQ(sink.hits, 1UL);
    ASSERT_EQ(sink.last.length, sizeof(kAsk));
    ASSERT_EQ(sink.copy[0], 7U);
    ASSERT_EQ(sink.copy[1], 8U);
    ASSERT_EQ(sink.copy[2], 9U);
    ASSERT_EQ(sink.last.sender_port, 40000U);
    ASSERT_EQ(sink.last.sender, cinux::net::kLoopbackAddress);
    ASSERT_EQ(wiring.udp.stats().delivered, 1UL);
}

TEST("udp: a computed checksum of zero travels as 0xffff") {
    Wiring wiring;

    const uint16_t kMagicPort                         = 0x01DC;
    unsigned char  probe[cinux::net::kUdpHeaderBytes] = {};
    cinux::base::WriteWord16Be(probe, 0, 0xFFFF);
    cinux::base::WriteWord16Be(probe, 2, kMagicPort);
    cinux::base::WriteWord16Be(probe, 4, cinux::net::kUdpHeaderBytes);
    ASSERT_EQ(pseudo_udp_checksum(cinux::net::kLoopbackAddress, cinux::net::kLoopbackAddress, probe,
                                  cinux::net::kUdpHeaderBytes),
              0U);

    ASSERT_TRUE(wiring.udp.send(cinux::net::kLoopbackAddress, 0xFFFF, kMagicPort, nullptr, 0));
    cinux::driver::net::RxFrame frame = {};
    ASSERT_TRUE(cinux::driver::net::Loopback::self().poll_frame(frame));
    const unsigned char* udp = frame.bytes + cinux::net::kMinIpv4HeaderBytes;
    ASSERT_EQ(cinux::base::ReadWord16Be(udp, 6), 0xFFFFU);
}

int main() {
    return cinux::test::RunAll();
}
