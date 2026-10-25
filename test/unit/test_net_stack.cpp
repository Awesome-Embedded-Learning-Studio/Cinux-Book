#include <stdint.h>

#include <cstddef>

#include "../framework/framework.hpp"
#include "cinux/assert.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/net/net_stack.hpp"
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

struct CannedNic {
    cinux::driver::net::RxFrame canned = {};
    bool                        served = false;

    [[nodiscard]] bool                           has_mac() const { return true; }
    [[nodiscard]] cinux::driver::net::MacAddress mac() const {
        return cinux::driver::net::MacAddress{};
    }
    [[nodiscard]] bool          has_ethernet_header() const { return true; }
    [[nodiscard]] unsigned long max_frame() const { return 1500; }
    [[nodiscard]] bool          poll_frame(cinux::driver::net::RxFrame& into) {
        if (served) {
            return false;
        }
        served = true;
        into   = canned;
        return true;
    }
    [[nodiscard]] bool send_l3(const cinux::driver::net::MacAddress& /*destination*/,
                               cinux::driver::net::EtherType /*type*/,
                               const unsigned char* /*payload*/, unsigned long /*length*/) {
        return true;
    }
};

struct Wiring {
    cinux::driver::net::NetPort port;
    cinux::net::NetStack        stack;
    cinux::net::Ipv4Module      ipv4;
    cinux::net::IcmpModule      icmp;

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
        icmp.init(kViaIpv4);
        cinux::net::L4Seat icmp_seat;
        icmp_seat.adopt(icmp);
        ipv4.seat_protocol(cinux::net::kIpv4ProtocolIcmp, icmp_seat);
        cinux::net::ProtocolSeat ipv4_seat;
        ipv4_seat.adopt(ipv4);
        stack.seat_protocol(cinux::net::kEtherTypeIpv4, ipv4_seat);
    }
};

void fill_echo_request(unsigned char* message, size_t length) {
    message[0] = cinux::net::kIcmpEchoRequest;
    message[1] = 0;
    cinux::base::WriteWord16Be(message, 2, 0);
    cinux::base::WriteWord16Be(message, 4, 0x1234);
    cinux::base::WriteWord16Be(message, 6, 0x0001);
    for (size_t index = cinux::net::kIcmpEchoHeaderBytes; index < length; ++index) {
        message[index] = static_cast<unsigned char>(index);
    }
    cinux::base::InternetChecksum checksum;
    checksum.add(message, length);
    cinux::base::WriteWord16Be(message, 2, checksum.value());
}

void fill_ipv4_header(unsigned char* datagram, unsigned char protocol, size_t total) {
    datagram[0] = 0x45;
    datagram[1] = 0;
    cinux::base::WriteWord16Be(datagram, 2, static_cast<uint16_t>(total));
    cinux::base::WriteWord16Be(datagram, 4, 0);
    cinux::base::WriteWord16Be(datagram, 6, 0);
    datagram[8] = cinux::net::kIpv4TimeToLive;
    datagram[9] = protocol;
    cinux::base::WriteWord16Be(datagram, 10, 0);
    cinux::base::WriteWord32Be(datagram, 12, cinux::net::kLoopbackAddress);
    cinux::base::WriteWord32Be(datagram, 16, cinux::net::kLoopbackAddress);
    cinux::base::InternetChecksum checksum;
    checksum.add(datagram, cinux::net::kMinIpv4HeaderBytes);
    cinux::base::WriteWord16Be(datagram, 10, checksum.value());
}

}  // namespace

TEST("stack: a loopback ping round trip drains inside one poll") {
    Wiring        wiring;
    unsigned char request[12];
    fill_echo_request(request, sizeof(request));
    ASSERT_TRUE(wiring.ipv4.send({.destination = cinux::net::kLoopbackAddress,
                                  .protocol    = cinux::net::kIpv4ProtocolIcmp,
                                  .payload     = request,
                                  .length      = sizeof(request)}));
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.icmp.stats().requests_answered == 1);
    ASSERT_TRUE(wiring.icmp.stats().replies_seen == 1);
    ASSERT_TRUE(wiring.icmp.stats().last_identifier == 0x1234);
    ASSERT_TRUE(wiring.icmp.stats().last_sequence == 0x0001);
    ASSERT_TRUE(wiring.ipv4.stats().delivered == 2);
    ASSERT_TRUE(wiring.ipv4.stats().sent == 2);
    ASSERT_TRUE(wiring.stack.stats().delivered == 2);
}

TEST("stack: an echo request past the mirror ceiling is refused by name") {
    Wiring        wiring;
    unsigned char request[cinux::net::kMaxIcmpEchoBytes + 4];
    fill_echo_request(request, sizeof(request));
    const cinux::net::Ipv4View kView = {
        .header_bytes = cinux::net::kMinIpv4HeaderBytes,
        .total_length = static_cast<uint16_t>(sizeof(request)),
        .protocol     = cinux::net::kIpv4ProtocolIcmp,
        .source       = cinux::net::kLoopbackAddress,
        .destination  = cinux::net::kLoopbackAddress,
    };
    wiring.icmp.handle(request, sizeof(request), kView);
    ASSERT_TRUE(wiring.icmp.stats().refused_oversized == 1);
    ASSERT_TRUE(wiring.icmp.stats().requests_answered == 0);
}

TEST("stack: a header lying about its own length dies at the ipv4 door") {
    Wiring        wiring;
    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(datagram, cinux::net::kIpv4ProtocolIcmp, sizeof(datagram));
    datagram[0] = 0x4F;
    wiring.ipv4.handle(datagram, sizeof(datagram));
    ASSERT_TRUE(wiring.ipv4.stats().refused_short == 1);
    ASSERT_TRUE(wiring.ipv4.stats().refused_checksum == 0);
}

TEST("stack: the frame budget is the re-entry leash") {
    Wiring        wiring;
    unsigned char request[12];
    fill_echo_request(request, sizeof(request));
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(wiring.ipv4.send({.destination = cinux::net::kLoopbackAddress,
                                      .protocol    = cinux::net::kIpv4ProtocolIcmp,
                                      .payload     = request,
                                      .length      = sizeof(request)}));
    }
    wiring.stack.poll(2);
    ASSERT_TRUE(wiring.stack.stats().delivered == 2);
    ASSERT_TRUE(wiring.icmp.stats().requests_answered == 2);
    ASSERT_TRUE(wiring.icmp.stats().replies_seen == 0);
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.icmp.stats().requests_answered == 3);
    ASSERT_TRUE(wiring.icmp.stats().replies_seen == 3);
}

TEST("ipv4: a header whose checksum does not fold to zero dies counted") {
    Wiring        wiring;
    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(datagram, cinux::net::kIpv4ProtocolIcmp, cinux::net::kMinIpv4HeaderBytes);
    datagram[10] = ~datagram[10];
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        cinux::driver::net::MacAddress{}, cinux::driver::net::EtherType{cinux::net::kEtherTypeIpv4},
        datagram, sizeof(datagram)));
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.ipv4.stats().refused_checksum == 1);
    ASSERT_TRUE(wiring.ipv4.stats().delivered == 0);
}

TEST("ipv4: a protocol nobody seated dies counted") {
    Wiring        wiring;
    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(datagram, 99, cinux::net::kMinIpv4HeaderBytes);
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        cinux::driver::net::MacAddress{}, cinux::driver::net::EtherType{cinux::net::kEtherTypeIpv4},
        datagram, sizeof(datagram)));
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.ipv4.stats().unknown_protocol == 1);
    ASSERT_TRUE(wiring.ipv4.stats().delivered == 0);
}

TEST("icmp: a message whose checksum does not fold to zero dies counted") {
    Wiring        wiring;
    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes + 12] = {};
    fill_echo_request(datagram + cinux::net::kMinIpv4HeaderBytes, 12);
    datagram[cinux::net::kMinIpv4HeaderBytes + 2] = ~datagram[cinux::net::kMinIpv4HeaderBytes + 2];
    fill_ipv4_header(datagram, cinux::net::kIpv4ProtocolIcmp, sizeof(datagram));
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        cinux::driver::net::MacAddress{}, cinux::driver::net::EtherType{cinux::net::kEtherTypeIpv4},
        datagram, sizeof(datagram)));
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.ipv4.stats().delivered == 1);
    ASSERT_TRUE(wiring.icmp.stats().refused_checksum == 1);
    ASSERT_TRUE(wiring.icmp.stats().requests_answered == 0);
}

TEST("icmp: a message short of eight bytes dies counted") {
    Wiring        wiring;
    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes + 7] = {};
    datagram[cinux::net::kMinIpv4HeaderBytes]                   = cinux::net::kIcmpEchoRequest;
    fill_ipv4_header(datagram, cinux::net::kIpv4ProtocolIcmp, sizeof(datagram));
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        cinux::driver::net::MacAddress{}, cinux::driver::net::EtherType{cinux::net::kEtherTypeIpv4},
        datagram, sizeof(datagram)));
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.icmp.stats().refused_short == 1);
}

TEST("stack: an ethernet frame short of fourteen bytes dies counted") {
    Wiring    wiring;
    CannedNic nic;
    nic.canned.size = cinux::net::kEthernetHeaderBytes - 1;
    cinux::driver::net::NetPort port;
    port.adopt(nic);
    wiring.stack.attach_port(port);
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.stack.stats().refused_ethernet == 1);
    ASSERT_TRUE(wiring.stack.stats().delivered == 0);
}

TEST("stack: an ethertype nobody seated dies counted") {
    Wiring    wiring;
    CannedNic nic;
    nic.canned.size      = cinux::net::kEthernetHeaderBytes + 2;
    nic.canned.bytes[12] = 0x99;
    nic.canned.bytes[13] = 0x99;
    nic.canned.bytes[14] = 0x00;
    nic.canned.bytes[15] = 0x00;
    cinux::driver::net::NetPort port;
    port.adopt(nic);
    wiring.stack.attach_port(port);
    wiring.stack.poll(cinux::net::kPollBudgetFrames);
    ASSERT_TRUE(wiring.stack.stats().unknown_protocol == 1);
    ASSERT_TRUE(wiring.stack.stats().delivered == 0);
}

int main() {
    return cinux::test::RunAll();
}
