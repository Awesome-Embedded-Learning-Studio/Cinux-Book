#include <stddef.h>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "framework_kernel.hpp"
#include "kernel/driver/net/e1000.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_world.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

bool g_world_wired = false;

void ensure_world() {
    if (g_world_wired) {
        return;
    }
    cinux::net::NetWorld::self().init();
    g_world_wired = true;
}

void fill_echo_request(unsigned char* message, size_t length) {
    message[0] = cinux::net::kIcmpEchoRequest;
    message[1] = 0;
    cinux::base::WriteWord16Be(message, 2, 0);
    cinux::base::WriteWord16Be(message, 4, 0x3030);
    cinux::base::WriteWord16Be(message, 6, 0x0001);
    for (size_t index = cinux::net::kIcmpEchoHeaderBytes; index < length; ++index) {
        message[index] = static_cast<unsigned char>(index);
    }
    cinux::base::InternetChecksum checksum;
    checksum.add(message, length);
    cinux::base::WriteWord16Be(message, 2, checksum.value());
}

}  // namespace

TEST("net: e1000 answers a ping through the user network") {
    ensure_world();
    ASSERT_TRUE(cinux::driver::net::E1000::self().init());
    cinux::net::NetWorld::self().attach_wire(cinux::driver::net::E1000::self().port());
    const unsigned long kReplies =
        cinux::net::NetWorld::self().ping_sweep(cinux::net::kUserNetworkHost, 4);
    ASSERT_TRUE(kReplies != 0);
}

TEST("net: hand-cranking the pump answers a loopback ping") {
    ensure_world();
    unsigned char request[12] = {};
    fill_echo_request(request, sizeof(request));
    ASSERT_TRUE(
        cinux::net::NetWorld::self().ipv4().send({.destination = cinux::net::kLoopbackAddress,
                                                  .protocol    = cinux::net::kIpv4ProtocolIcmp,
                                                  .payload     = request,
                                                  .length      = sizeof(request)}));

    cinux::net::NetWorld::self().pump_once();
    ASSERT_TRUE(cinux::net::NetWorld::self().icmp().stats().replies_seen == 1);
    ASSERT_TRUE(cinux::net::NetWorld::self().icmp().stats().requests_answered == 1);
    ASSERT_TRUE(cinux::net::NetWorld::self().icmp().stats().last_identifier == 0x3030);
    ASSERT_TRUE(cinux::net::NetWorld::self().icmp().stats().last_sequence == 0x0001);
}

TEST("net: a checksum mismatch dies counted in the kernel world") {
    ensure_world();
    const unsigned long kDeliveredBefore = cinux::net::NetWorld::self().ipv4().stats().delivered;

    unsigned char datagram[cinux::net::kMinIpv4HeaderBytes] = {};
    datagram[0]                                             = 0x45;
    cinux::base::WriteWord16Be(datagram, 2, cinux::net::kMinIpv4HeaderBytes);
    datagram[8] = cinux::net::kIpv4TimeToLive;
    datagram[9] = cinux::net::kIpv4ProtocolIcmp;
    cinux::base::WriteWord32Be(datagram, 12, cinux::net::kLoopbackAddress);
    cinux::base::WriteWord32Be(datagram, 16, cinux::net::kLoopbackAddress);
    datagram[10] = 0xde;
    datagram[11] = 0xad;

    const cinux::driver::net::MacAddress kNobody = {};
    const cinux::driver::net::EtherType  kIpv4{cinux::net::kEtherTypeIpv4};
    ASSERT_TRUE(
        cinux::driver::net::Loopback::self().send_l3(kNobody, kIpv4, datagram, sizeof(datagram)));

    cinux::net::NetWorld::self().pump_once();
    ASSERT_TRUE(cinux::net::NetWorld::self().ipv4().stats().refused_checksum != 0);
    ASSERT_TRUE(cinux::net::NetWorld::self().ipv4().stats().delivered == kDeliveredBefore);
}
