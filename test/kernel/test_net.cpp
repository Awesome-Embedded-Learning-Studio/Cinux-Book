#include <stddef.h>

#include "cinux/byte_order.hpp"
#include "cinux/checksum.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/net/e1000.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_world.hpp"
#include "kernel/net/tcp.hpp"
#include "kernel/syscall/sys_clock.hpp"
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

struct TcpNews {
    unsigned long accepted_hits = 0;
    unsigned long data_hits     = 0;
    unsigned long graceful      = 0;

    static void on_accepted(void* context) { ++static_cast<TcpNews*>(context)->accepted_hits; }
    static void on_data(void* context) { ++static_cast<TcpNews*>(context)->data_hits; }
    static void on_farewell(void* context, cinux::net::TcpFarewell farewell) {
        if (farewell == cinux::net::TcpFarewell::kGraceful) {
            ++static_cast<TcpNews*>(context)->graceful;
        }
    }
};

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("net: a tcp handshake carries bytes and completes the farewell") {
    ensure_world();
    auto& world = cinux::net::NetWorld::self();
    auto& tcp   = world.tcp();

    TcpNews                     news;
    const cinux::net::TcpEvents kServerEvents = {
        .context  = &news,
        .data     = &TcpNews::on_data,
        .accepted = &TcpNews::on_accepted,
        .farewell = &TcpNews::on_farewell,
    };
    const auto kListener = tcp.listen(4301, kServerEvents);
    ASSERT_TRUE(kListener.ok());

    TcpNews                     client_news;
    const cinux::net::TcpEvents kClientEvents = {
        .context  = &client_news,
        .data     = &TcpNews::on_data,
        .accepted = &TcpNews::on_accepted,
        .farewell = &TcpNews::on_farewell,
    };
    const auto kClient = tcp.connect(0, cinux::net::kLoopbackAddress, 4301, kClientEvents, 0);
    ASSERT_TRUE(kClient.ok());

    world.pump_once();

    ASSERT_TRUE(tcp.established(kClient.value()));
    ASSERT_TRUE(news.accepted_hits == 1);
    const auto kServer = tcp.take_accepted(kListener.value());
    ASSERT_TRUE(kServer.ok());

    const unsigned char kPing[4] = {'p', 'i', 'n', 'g'};
    const auto          kGiven   = tcp.send(kClient.value(), kPing, sizeof(kPing), 0);
    ASSERT_TRUE(kGiven.ok());
    ASSERT_TRUE(kGiven.value() == sizeof(kPing));
    world.pump_once();
    ASSERT_TRUE(news.data_hits == 1);
    unsigned char inbox[16] = {};
    const auto    kTaken    = tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_TRUE(kTaken.ok());
    ASSERT_TRUE(kTaken.value() == sizeof(kPing));

    tcp.close(kClient.value());
    world.pump_once();
    const auto kEof = tcp.receive(kServer.value(), inbox, sizeof(inbox));
    ASSERT_TRUE(kEof.ok());
    ASSERT_TRUE(kEof.value() == 0);

    tcp.close(kServer.value());
    world.pump_once();
    const unsigned long long kDeadline =
        cinux::syscall::MonotonicNanoseconds() + (4 * cinux::net::kTcpMslNanoseconds);
    while (client_news.graceful == 0 && cinux::syscall::MonotonicNanoseconds() < kDeadline) {
        world.pump_once();
        cinux::arch::EnableIrqAndHalt();
    }
    ASSERT_TRUE(client_news.graceful == 1);
    ASSERT_TRUE(news.graceful == 1);
}
