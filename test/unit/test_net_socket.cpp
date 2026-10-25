#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <thread>

#include "../framework/framework.hpp"
#include "cinux/assert.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/local_config.hpp"
#include "kernel/net/local_socket.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/net_world.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/net/tcp.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_ioctl.hpp"
#include "kernel/syscall/sys_itimer.hpp"
#include "kernel/syscall/sys_socket.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "test_assert.hpp"

namespace cinux::syscall {

// NOLINTNEXTLINE(misc-use-internal-linkage) the link stub must stay external
unsigned long long g_test_monotonic_ns      = 0;
// NOLINTNEXTLINE(misc-use-internal-linkage) the link stub must stay external
unsigned long long g_test_monotonic_step_ns = 0;

// NOLINTNEXTLINE(misc-use-internal-linkage) the link stub must stay external
unsigned long long MonotonicNanoseconds() {
    const auto kNow = g_test_monotonic_ns;
    g_test_monotonic_ns += g_test_monotonic_step_ns;
    return kNow;
}

}  // namespace cinux::syscall

namespace cinux::console {

// NOLINTNEXTLINE(misc-use-internal-linkage) the link stub must stay external
void PutString(const char* /*text*/, bool /*newline*/) {}

}  // namespace cinux::console

namespace cinux::proc {

TaskBuilder& TaskBuilder::set_entry(void (* /*entry*/)()) {
    return *this;
}

TaskBuilder& TaskBuilder::set_name(const char* /*name*/) {
    return *this;
}

TaskBuilder& TaskBuilder::set_priority(unsigned char /*priority*/) {
    return *this;
}

Task* TaskBuilder::build() {
    return nullptr;
}

}  // namespace cinux::proc

namespace {

using cinux::syscall::Word;

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

void (*g_socket_body)() = nullptr;

class SocketSink {
public:
    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void enter_from_main(cinux::proc::Task& incoming);
    void return_to_main(cinux::proc::Task& outgoing);
    void reclaim(cinux::proc::Task& dead);

private:
    void run_body_of(cinux::proc::Task& incoming);
};

class SocketWorld {
public:
    SocketWorld() {
        cinux::net::NetWorld::self().init();
        kin.name          = "kin";
        runner.name       = "face";
        runner.stack_base = 0x1000ULL * 0x40;
        runner.parent     = &kin;
        cinux::proc::Scheduler::self().init(sink);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
        cinux::proc::Scheduler::self().seat(runner);
    }

    ~SocketWorld() {
        cinux::proc::Scheduler::self().seat(kin);
        cinux::proc::Scheduler::self().run_until_done();
    }

    void run(void (*body)()) {
        g_socket_body = body;
        cinux::proc::Scheduler::self().run_until_done();
    }

    SocketSink        sink;
    cinux::proc::Task kin;
    cinux::proc::Task runner;
};

void SocketSink::run_body_of([[maybe_unused]] cinux::proc::Task& incoming) {
    if (g_socket_body != nullptr) {
        void (*const kBody)() = g_socket_body;
        g_socket_body         = nullptr;
        kBody();
        return;
    }
    cinux::proc::Scheduler::self().exit_current();
}

void SocketSink::enter_from_main(cinux::proc::Task& incoming) {
    run_body_of(incoming);
}

void SocketSink::switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming) {
    static_cast<void>(outgoing);
    run_body_of(incoming);
}

void SocketSink::return_to_main(cinux::proc::Task& outgoing) {
    static_cast<void>(outgoing);
}

void SocketSink::reclaim(cinux::proc::Task& dead) {
    static_cast<void>(dead);
}

cinux::syscall::SyscallFrame plain_frame() {
    return cinux::syscall::SyscallFrame{};
}

void fill_sockaddr(unsigned char* record, uint32_t address, uint16_t port) {
    const auto kFamily = static_cast<uint16_t>(cinux::net::kAfInet);
    cinux::base::CopyBytes(record, &kFamily, sizeof(kFamily));
    cinux::base::WriteWord16Be(record, 2, port);
    cinux::base::WriteWord32Be(record, 4, address);
}

struct StreamWiring {
    cinux::driver::net::NetPort port;
    cinux::net::NetStack        stack;
    cinux::net::Ipv4Module      ipv4;
    cinux::net::TcpModule       tcp;

    StreamWiring() {
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

// A second interface is present, but no loopback traffic should reach it.
struct SilentWire {
    unsigned long                  sent = 0;
    bool                           has_mac() { return true; }
    cinux::driver::net::MacAddress mac() { return {}; }
    bool                           has_ethernet_header() { return true; }
    unsigned long                  max_frame() { return cinux::driver::net::kMaxEthernetFrame; }
    bool                           poll_frame(cinux::driver::net::RxFrame& frame) {
        static_cast<void>(frame);
        return false;
    }
    bool send_l3(const cinux::driver::net::MacAddress& peer, cinux::driver::net::EtherType type,
                 const uint8_t* payload, unsigned long length) {
        static_cast<void>(peer);
        static_cast<void>(type);
        static_cast<void>(payload);
        static_cast<void>(length);
        ++sent;
        return true;
    }
};

}  // namespace


// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("stream socket: a loopback session flows both ways through accept") {
    StreamWiring                    wiring;
    cinux::net::TcpSocket           server(wiring.tcp);
    const cinux::net::SocketAddress kSeat{
        .address = cinux::net::kLoopbackAddress,
        .port    = 4400,
    };
    ASSERT_TRUE(server.adopt_local(kSeat).ok());
    ASSERT_TRUE(server.begin_listen(2).ok());

    cinux::net::TcpSocket client(wiring.tcp);
    ASSERT_TRUE(client.adopt_peer(kSeat).ok());
    ASSERT_TRUE(client.handshake_pending());
    wiring.stack.poll(16);
    ASSERT_FALSE(client.handshake_pending());

    const auto kBorn = server.accept_one();
    ASSERT_TRUE(kBorn.ok());
    const unsigned char kHi[3] = {'h', 'i', '!'};
    const auto          kSent  = client.transmit(kHi, sizeof(kHi), kSeat);
    ASSERT_EQ(kSent.value(), sizeof(kHi));
    wiring.stack.poll(16);
    unsigned char got[8] = {};
    const auto    kHeard = kBorn.value()->receive(got, sizeof(got), nullptr);
    ASSERT_EQ(kHeard.value(), sizeof(kHi));
    ASSERT_EQ(got[0], static_cast<unsigned char>('h'));

    const unsigned char kYo[2] = {'y', 'o'};
    const auto          kEcho  = kBorn.value()->transmit(kYo, sizeof(kYo), kSeat);
    ASSERT_EQ(kEcho.value(), sizeof(kYo));
    wiring.stack.poll(16);
    const auto kBack = client.receive(got, sizeof(got), nullptr);
    ASSERT_EQ(kBack.value(), sizeof(kYo));
    ASSERT_EQ(got[1], static_cast<unsigned char>('o'));
    ASSERT_TRUE(kBorn.value()->shutdown_write().ok());
    wiring.stack.poll(16);
    // A peer FIN can beat the next connect() poll; CLOSE_WAIT is not
    // an unfinished handshake.
    ASSERT_FALSE(client.handshake_pending());
    ASSERT_FALSE(client.handshake_failed());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("stream socket: attaching Ethernet preserves loopback handshake and payload") {
    using namespace cinux;
    auto& world = net::NetWorld::self();
    world.init();
    SilentWire           wire;
    driver::net::NetPort port;
    port.adopt(wire);
    world.attach_wire(port);
    // IPv4 and UDP must agree on the selected source in the pseudo header.
    net::UdpSocket udp_server(world.udp());
    net::UdpSocket udp_client(world.udp());
    ASSERT_TRUE(udp_server.adopt_local({.port = 4413}).ok());
    const unsigned char kByte = 42;
    ASSERT_TRUE(
        udp_client.transmit(&kByte, 1, {.address = net::kLoopbackAddress, .port = 4413}).ok());
    world.pump_once();
    unsigned char      byte = 0;
    net::SocketAddress from;
    const auto         kUdpRead = udp_server.receive(&byte, 1, &from);
    ASSERT_TRUE(kUdpRead.ok());
    ASSERT_EQ(byte, kByte);
    ASSERT_EQ(from.address, net::kLoopbackAddress);
    net::TcpSocket server(world.tcp());
    ASSERT_TRUE(server.adopt_local({.port = 4410}).ok());
    ASSERT_TRUE(server.begin_listen(2).ok());
    net::TcpSocket client(world.tcp());
    // BusyBox nc binds INADDR_ANY:0 before its active open.
    ASSERT_TRUE(client.adopt_local({}).ok());
    ASSERT_TRUE(client.adopt_peer({.address = net::kLoopbackAddress, .port = 4410}).ok());
    world.pump_once();
    ASSERT_FALSE(client.handshake_pending());
    ASSERT_FALSE(client.handshake_failed());
    const auto kBorn = server.accept_one();
    ASSERT_TRUE(kBorn.ok());
    ASSERT_EQ(wire.sent, 0UL);
    if (kBorn.ok()) {
        const unsigned char kHi[] = {'h', 'i', '\n'};
        ASSERT_TRUE(client.transmit(kHi, sizeof(kHi), {}).ok());
        world.pump_once();
        unsigned char got[8]{};
        const auto    kRead = kBorn.value()->receive(got, sizeof(got), nullptr);
        ASSERT_TRUE(kRead.ok());
        if (kRead.ok()) {
            ASSERT_EQ(kRead.value(), sizeof(kHi));
            ASSERT_EQ(got[0], static_cast<unsigned char>('h'));
        }
    }
    ASSERT_EQ(wire.sent, 0UL);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("stream socket: writer exit before accept preserves queued bytes and EOF") {
    using namespace cinux;
    StreamWiring             wiring;
    net::TcpSocket           server(wiring.tcp);
    const net::SocketAddress kSeat{.address = net::kLoopbackAddress, .port = 4412};
    ASSERT_TRUE(server.adopt_local(kSeat).ok());
    ASSERT_TRUE(server.begin_listen(2).ok());
    {
        net::TcpSocket client(wiring.tcp);
        ASSERT_TRUE(client.adopt_peer(kSeat).ok());
        wiring.stack.poll(16);
        const unsigned char kHi[] = {'h', 'i', '\n'};
        ASSERT_TRUE(client.transmit(kHi, sizeof(kHi), {}).ok());
    }
    wiring.stack.poll(16);
    const auto kBorn = server.accept_one();
    ASSERT_TRUE(kBorn.ok());
    unsigned char got[8]{};
    const auto    kRead = kBorn.value()->receive(got, sizeof(got), nullptr);
    ASSERT_TRUE(kRead.ok());
    ASSERT_EQ(kRead.value(), 3UL);
    ASSERT_EQ(got[0], static_cast<unsigned char>('h'));
    const auto kEof = kBorn.value()->receive(got, sizeof(got), nullptr);
    ASSERT_TRUE(kEof.ok());
    ASSERT_EQ(kEof.value(), 0UL);
    ASSERT_EQ(wiring.tcp.stats().resets_sent, 0UL);
}

TEST("socket adapter: bind seats a port and getsockname answers it") {
    Wiring                          wiring;
    cinux::net::UdpSocket           socket(wiring.udp);
    const cinux::net::SocketAddress kAny{};

    ASSERT_TRUE(socket.adopt_local(kAny).ok());
    cinux::net::SocketAddress seated{};
    ASSERT_TRUE(socket.report_local(seated).ok());
    ASSERT_EQ(seated.port, cinux::net::kEphemeralPortFirst);
    ASSERT_EQ(seated.address, cinux::net::kLoopbackAddress);

    cinux::net::UdpSocket twin(wiring.udp);
    ASSERT_FALSE(twin.adopt_local(seated).ok());
}

TEST("socket adapter: a datagram crosses the loopback and keeps its sender") {
    Wiring                wiring;
    cinux::net::UdpSocket sender(wiring.udp);
    cinux::net::UdpSocket hearer(wiring.udp);
    ASSERT_TRUE(hearer.adopt_local(cinux::net::SocketAddress{.address = 0, .port = 5353}).ok());

    const unsigned char kAsk[3] = {7, 8, 9};
    ASSERT_TRUE(sender
                    .transmit(kAsk, sizeof(kAsk),
                              cinux::net::SocketAddress{.address = cinux::net::kLoopbackAddress,
                                                        .port    = 5353})
                    .ok());
    wiring.stack.poll(4);

    ASSERT_TRUE(hearer.holds_input());
    unsigned char             room[16] = {};
    cinux::net::SocketAddress from{};
    const auto                kTaken = hearer.receive(room, sizeof(room), &from);
    ASSERT_TRUE(kTaken.ok());
    ASSERT_EQ(kTaken.value(), 3UL);
    ASSERT_EQ(room[0], 7U);
    ASSERT_EQ(room[2], 9U);
    ASSERT_EQ(from.address, cinux::net::kLoopbackAddress);
}

TEST("socket adapter: UDP payload leaves room for both IPv4 and UDP headers") {
    static_assert(cinux::net::kUdpSocketPayloadBytes == 1472);
    Wiring                          wiring;
    cinux::net::UdpSocket           sender(wiring.udp);
    cinux::net::UdpSocket           receiver(wiring.udp);
    const cinux::net::SocketAddress kLocal{.address = cinux::net::kLoopbackAddress, .port = 5353};
    ASSERT_TRUE(receiver.adopt_local(kLocal).ok());
    unsigned char bytes[cinux::net::kUdpSocketPayloadBytes + 1]{};
    ASSERT_FALSE(sender.transmit(bytes, sizeof(bytes), kLocal).ok());
    ASSERT_TRUE(sender.transmit(bytes, sizeof(bytes) - 1, kLocal).ok());
    wiring.stack.poll(4);
    const auto kReceived = receiver.receive(bytes, sizeof(bytes), nullptr);
    ASSERT_TRUE(kReceived.ok());
    ASSERT_EQ(kReceived.value(), sizeof(bytes) - 1);
}

TEST("socket adapter: an empty ring answers would-block, a short read truncates") {
    Wiring                wiring;
    cinux::net::UdpSocket socket(wiring.udp);
    ASSERT_TRUE(socket.adopt_local(cinux::net::SocketAddress{.address = 0, .port = 7001}).ok());

    unsigned char room[8] = {};
    const auto    kEmpty  = socket.receive(room, sizeof(room), nullptr);
    ASSERT_EQ(kEmpty.error(), cinux::base::KernelError::kWouldBlock);

    cinux::net::UdpSocket sender(wiring.udp);
    const unsigned char   kAsk[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(sender
                    .transmit(kAsk, sizeof(kAsk),
                              cinux::net::SocketAddress{.address = cinux::net::kLoopbackAddress,
                                                        .port    = 7001})
                    .ok());
    wiring.stack.poll(4);
    const auto kShort = socket.receive(room, 3, nullptr);
    ASSERT_TRUE(kShort.ok());
    ASSERT_EQ(kShort.value(), 3UL);
}

TEST("socket adapter: a full ring drops the newcomer and counts it") {
    Wiring                wiring;
    cinux::net::UdpSocket hearer(wiring.udp);
    ASSERT_TRUE(hearer.adopt_local(cinux::net::SocketAddress{.address = 0, .port = 7002}).ok());
    cinux::net::UdpSocket sender(wiring.udp);

    const unsigned char kOne[2] = {0xAA, 0x55};
    for (unsigned long round = 0; round <= cinux::net::kUdpSocketQueueDatagrams; ++round) {
        ASSERT_TRUE(sender
                        .transmit(kOne, sizeof(kOne),
                                  cinux::net::SocketAddress{.address = cinux::net::kLoopbackAddress,
                                                            .port    = 7002})
                        .ok());
        wiring.stack.poll(4);
    }
    ASSERT_EQ(hearer.dropped(), 1UL);
}

TEST("socket adapter: two sockets on two ports never hear each other") {
    Wiring                wiring;
    cinux::net::UdpSocket first(wiring.udp);
    cinux::net::UdpSocket second(wiring.udp);
    ASSERT_TRUE(first.adopt_local(cinux::net::SocketAddress{.address = 0, .port = 7010}).ok());
    ASSERT_TRUE(second.adopt_local(cinux::net::SocketAddress{.address = 0, .port = 7011}).ok());
    cinux::net::UdpSocket sender(wiring.udp);

    const unsigned char kAsk[2] = {3, 3};
    ASSERT_TRUE(sender
                    .transmit(kAsk, sizeof(kAsk),
                              cinux::net::SocketAddress{.address = cinux::net::kLoopbackAddress,
                                                        .port    = 7011})
                    .ok());
    wiring.stack.poll(4);
    ASSERT_FALSE(first.holds_input());
    ASSERT_TRUE(second.holds_input());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("socket adapter: the erased face reads, writes, and stats a socket") {
    Wiring      wiring;
    auto* const kHearer = new cinux::net::UdpSocket(wiring.udp);
    ASSERT_TRUE(kHearer->adopt_local(cinux::net::SocketAddress{.address = 0, .port = 7020}).ok());
    auto* const kSender = new cinux::net::UdpSocket(wiring.udp);
    ASSERT_TRUE(kSender
                    ->adopt_peer(cinux::net::SocketAddress{.address = cinux::net::kLoopbackAddress,
                                                           .port    = 7020})
                    .ok());

    const cinux::fs::FsOps* ops   = cinux::net::SocketOps();
    const cinux::fs::FsNode kNode = {
        .opaque = kSender,
        .type   = cinux::fs::InodeType::kFile,
        .ino    = kSender->identity(),
        .size   = 0,
    };
    const unsigned char kAsk[3] = {9, 9, 9};
    const auto          kWrote  = ops->write(kSender, kNode, 0, kAsk, sizeof(kAsk));
    ASSERT_TRUE(kWrote.ok());
    ASSERT_EQ(kWrote.value(), 3UL);
    wiring.stack.poll(4);

    cinux::fs::FsStat       probed{};
    const cinux::fs::FsNode kHearerNode = {
        .opaque = kHearer,
        .type   = cinux::fs::InodeType::kFile,
        .ino    = kHearer->identity(),
        .size   = 0,
    };
    ASSERT_TRUE(ops->stat(kHearer, kHearerNode, probed).ok());
    ASSERT_EQ(probed.mode, 0140600U);
    ASSERT_TRUE(ops->readable(kHearer, kHearerNode));
    ASSERT_FALSE(ops->readable(kSender, kNode));

    unsigned char room[8] = {};
    const auto    kRead   = ops->read(kHearer, kHearerNode, 0, room, sizeof(room));
    ASSERT_TRUE(kRead.ok());
    ASSERT_EQ(kRead.value(), 3UL);
    ASSERT_EQ(room[0], 9U);

    ops->retain(kHearer, kHearerNode);
    if (kHearer->release()) {
        delete kHearer;
    }
    if (kSender->release()) {
        delete kSender;
    }
}

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_socket_shapes_body() {
    cinux::syscall::SyscallFrame frame = plain_frame();
    frame.rdi                          = 0;
    frame.rsi                          = 1;
    frame.rdx                          = 0;
    ASSERT_EQ(cinux::syscall::HandleSocketpair(frame), -cinux::syscall::kEafnosupport);

    frame                    = plain_frame();
    frame.rdi                = cinux::net::kAfInet;
    frame.rsi                = 4;
    const long long kBadType = cinux::syscall::HandleSocket(frame);
    ASSERT_EQ(kBadType, -cinux::syscall::kEsocktnosupport);
    frame.rsi = cinux::net::kSockStream;
    frame.rdx = static_cast<unsigned long long>(cinux::net::kIpv4ProtocolUdp);
    ASSERT_EQ(cinux::syscall::HandleSocket(frame), -cinux::syscall::kEprotonosupport);
    frame.rdx               = static_cast<unsigned long long>(cinux::net::kIpv4ProtocolTcp);
    const long long kStream = cinux::syscall::HandleSocket(frame);
    ASSERT_TRUE(kStream >= 0);

    frame.rdx           = 0;
    const long long kFd = cinux::syscall::HandleSocket(frame);
    ASSERT_TRUE(kFd >= 0);

    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kFd);
    ASSERT_EQ(cinux::syscall::HandleListen(frame), -cinux::syscall::kEinval);
    ASSERT_EQ(cinux::syscall::HandleAccept(frame), -cinux::syscall::kEopnotsupp);
    ASSERT_EQ(cinux::syscall::HandleAccept4(frame), -cinux::syscall::kEopnotsupp);
    frame.rsi = 1;
    ASSERT_EQ(cinux::syscall::HandleShutdown(frame), -cinux::syscall::kEnotconn);

    frame.rsi           = cinux::syscall::kSolSocket;
    frame.rdx           = cinux::syscall::kSoType;
    unsigned int answer = 0;
    uint32_t     length = sizeof(answer);
    frame.r10           = reinterpret_cast<Word>(&answer);
    frame.r8            = reinterpret_cast<Word>(&length);
    ASSERT_EQ(cinux::syscall::HandleGetsockopt(frame), 0);
    ASSERT_EQ(answer, static_cast<unsigned int>(cinux::net::kSockStream));

    frame.rdx = 999;
    ASSERT_EQ(cinux::syscall::HandleGetsockopt(frame), -cinux::syscall::kEnoprotoopt);
    frame.rdx = cinux::syscall::kSoRcvbuf;
    frame.r10 = reinterpret_cast<Word>(&answer);
    frame.r8  = reinterpret_cast<Word>(&length);
    ASSERT_EQ(cinux::syscall::HandleSetsockopt(frame), 0);
    frame.r10 = 0;
    ASSERT_EQ(cinux::syscall::HandleSetsockopt(frame), -cinux::syscall::kEfault);

    const auto kFiles = cinux::proc::Scheduler::self().current()->files;
    const auto kPlain = kFiles->alloc(nullptr, nullptr, cinux::fs::FsNode{});
    frame             = plain_frame();
    frame.rdi         = kPlain.value();
    ASSERT_EQ(cinux::syscall::HandleBind(frame), -cinux::syscall::kEnotsock);
    frame.rdi = 999;
    ASSERT_EQ(cinux::syscall::HandleBind(frame), -cinux::syscall::kEbadf);
    cinux::proc::Scheduler::self().exit_current();
}

}  // namespace

TEST("socket syscall: creation, shapes, and the not-socket answer") {
    SocketWorld world;
    world.run(run_socket_shapes_body);
}

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_socket_roundtrip_body() {
    unsigned char asked[16] = {};
    fill_sockaddr(asked, 0, 5353);

    cinux::syscall::SyscallFrame frame = plain_frame();
    frame.rdi                          = cinux::net::kAfInet;
    frame.rsi                          = cinux::net::kSockDgram;
    frame.rdx                          = 0;
    const long long kHearer            = cinux::syscall::HandleSocket(frame);
    ASSERT_TRUE(kHearer >= 0);

    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kHearer);
    frame.rsi = reinterpret_cast<Word>(asked);
    frame.rdx = sizeof(asked);
    ASSERT_EQ(cinux::syscall::HandleBind(frame), 0);

    unsigned char echoed[16] = {};
    fill_sockaddr(echoed, cinux::net::kLoopbackAddress, 5353);
    const unsigned char kAsk[3] = {4, 5, 6};
    frame                       = plain_frame();
    frame.rdi                   = static_cast<Word>(kHearer);
    frame.rsi                   = reinterpret_cast<Word>(kAsk);
    frame.rdx                   = sizeof(kAsk);
    frame.r10                   = 0;
    frame.r8                    = reinterpret_cast<Word>(echoed);
    frame.r9                    = sizeof(echoed);
    ASSERT_EQ(cinux::syscall::HandleSendto(frame), 3);
    cinux::net::NetWorld::self().pump_once();

    unsigned char room[16] = {};
    unsigned char from[16] = {};
    unsigned long from_len = sizeof(from);
    frame                  = plain_frame();
    frame.rdi              = static_cast<Word>(kHearer);
    frame.rsi              = reinterpret_cast<Word>(room);
    frame.rdx              = sizeof(room);
    frame.r10              = 0;
    frame.r8               = reinterpret_cast<Word>(from);
    frame.r9               = reinterpret_cast<Word>(&from_len);
    ASSERT_EQ(cinux::syscall::HandleRecvfrom(frame), 3);
    ASSERT_EQ(room[0], 4U);
    ASSERT_EQ(room[2], 6U);
    ASSERT_EQ(cinux::base::ReadWord16Be(from, 2), 5353U);
    ASSERT_EQ(cinux::base::ReadWord32Be(from, 4), cinux::net::kLoopbackAddress);
    ASSERT_EQ(from_len, 16UL);

    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kHearer);
    frame.rsi = reinterpret_cast<Word>(echoed);
    frame.rdx = reinterpret_cast<Word>(&from_len);
    ASSERT_EQ(cinux::syscall::HandleGetsockname(frame), 0);
    ASSERT_EQ(cinux::base::ReadWord16Be(echoed, 2), 5353U);
    ASSERT_EQ(cinux::base::ReadWord32Be(echoed, 4), cinux::net::kLoopbackAddress);
    cinux::proc::Scheduler::self().exit_current();
}

}  // namespace

TEST("socket syscall: sendto and recvfrom cross the wired world") {
    SocketWorld world;
    world.run(run_socket_roundtrip_body);
}

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_accept_flags_body() {
    using namespace cinux;
    auto frame           = plain_frame();
    frame.rdi            = net::kAfInet;
    frame.rsi            = static_cast<Word>(net::kSockStream) | 0x800ULL;
    const auto kListener = syscall::HandleSocket(frame);
    ASSERT_TRUE(kListener >= 0);
    unsigned char address[16]{};
    fill_sockaddr(address, 0, 4411);
    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kListener);
    frame.rsi = reinterpret_cast<Word>(address);
    frame.rdx = sizeof(address);
    ASSERT_EQ(syscall::HandleBind(frame), 0);
    frame.rsi = 2;
    ASSERT_EQ(syscall::HandleListen(frame), 0);
    frame.rsi                         = 0;
    frame.rdx                         = 0;
    // Bound the old implementation's erroneous wait, without real time passing.
    syscall::g_test_monotonic_step_ns = 121'000'000'000ULL;
    ASSERT_EQ(syscall::HandleAccept(frame), -syscall::kEagain);
    auto& task = *proc::Scheduler::self().current();
    task.files->get(static_cast<unsigned long>(kListener)).value()->status_flags = 0;
    ASSERT_TRUE(proc::SendSignal(task, syscall::kSigInt));
    ASSERT_EQ(syscall::HandleAccept(frame), -syscall::kEintr);
    frame.r10 = 0x800;
    ASSERT_EQ(syscall::HandleAccept4(frame), -syscall::kEintr);
    frame.r10 = 1;
    ASSERT_EQ(syscall::HandleAccept4(frame), -syscall::kEinval);
    syscall::g_test_monotonic_step_ns = 0;
    syscall::g_test_monotonic_ns      = 0;
    task.signals.reset();

    auto&                world = net::NetWorld::self();
    const net::TcpEvents kNoNews{};
    const auto           kClient = world.tcp().connect(0, net::kLoopbackAddress, 4411, kNoNews, 0);
    ASSERT_TRUE(kClient.ok());
    world.pump_once();
    frame.r10            = 0x80800;  // SOCK_NONBLOCK | SOCK_CLOEXEC on the new socket.
    const auto kAccepted = syscall::HandleAccept4(frame);
    ASSERT_TRUE(kAccepted >= 0);
    ASSERT_EQ(task.files->get(static_cast<unsigned long>(kAccepted)).value()->status_flags,
              syscall::kOpenNonblock);
    frame.rdi = static_cast<Word>(kAccepted);
    frame.rsi = 3;
    ASSERT_EQ(syscall::HandleShutdown(frame), -syscall::kEinval);
    frame.rsi = 1;
    ASSERT_EQ(syscall::HandleShutdown(frame), 0);
    world.pump_once();
    unsigned char bytes[8]{};
    const auto    kEof = world.tcp().receive(kClient.value(), bytes, sizeof(bytes));
    ASSERT_TRUE(kEof.ok());
    ASSERT_EQ(kEof.value(), 0UL);
    // The peer may still send after seeing EOF from this half of the stream.
    const unsigned char kReply = 17;
    ASSERT_TRUE(world.tcp().send(kClient.value(), &kReply, 1, 0).ok());
    world.pump_once();
    frame.rsi = reinterpret_cast<Word>(bytes);
    frame.rdx = sizeof(bytes);
    frame.r10 = 0;
    ASSERT_EQ(syscall::HandleRecvfrom(frame), 1);
    ASSERT_EQ(bytes[0], kReply);
    world.tcp().close(kClient.value());
    world.pump_once();
    proc::Scheduler::self().exit_current();
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
TEST("socket syscall: accept honors listener flags and signals; accept4 flags belong to child") {
    SocketWorld world;
    world.run(run_accept_flags_body);
}

namespace {

struct TimerImage {
    long long interval_seconds      = 0;
    long long interval_microseconds = 0;
    long long value_seconds         = 0;
    long long value_microseconds    = 0;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_timer_body() {
    using namespace cinux;
    auto& scheduler              = proc::Scheduler::self();
    auto& task                   = *scheduler.current();
    syscall::g_test_monotonic_ns = 5'000'000'000;
    auto frame                   = plain_frame();
    frame.rdi                    = 2;
    ASSERT_EQ(syscall::HandleAlarm(frame), 0);
    scheduler.on_timer_tick(6'999'999'999);
    ASSERT_FALSE(proc::HasPendingSignal(task));
    scheduler.on_timer_tick(7'000'000'000);
    ASSERT_TRUE(proc::HasPendingSignal(task));
    ASSERT_EQ(task.state, proc::TaskState::kRunning);
    task.signals.reset();
    TimerImage image{.interval_microseconds = 1000, .value_microseconds = 500};
    frame     = plain_frame();
    frame.rsi = reinterpret_cast<Word>(&image);
    ASSERT_EQ(syscall::HandleSetitimer(frame), 0);
    scheduler.on_timer_tick(5'010'000'000);
    ASSERT_TRUE(proc::HasPendingSignal(task));
    ASSERT_EQ(task.real_deadline_ns, 5'010'500'000ULL);
    TimerImage old{};
    frame.rsi                    = reinterpret_cast<Word>(&old);
    syscall::g_test_monotonic_ns = 5'010'000'000;
    ASSERT_EQ(syscall::HandleGetitimer(frame), 0);
    ASSERT_EQ(old.interval_microseconds, 1000LL);
    ASSERT_EQ(old.value_microseconds, 500LL);
    frame     = plain_frame();
    frame.rdi = 1;
    ASSERT_EQ(syscall::HandleAlarm(frame), 1);
    frame.rdi = 0;
    ASSERT_EQ(syscall::HandleAlarm(frame), 1);
    ASSERT_EQ(task.real_deadline_ns, 0ULL);
    ASSERT_EQ(task.real_interval_ns, 0ULL);
    image.value_microseconds = 1'000'000;
    frame.rsi                = reinterpret_cast<Word>(&image);
    ASSERT_EQ(syscall::HandleSetitimer(frame), -syscall::kEinval);
    frame.rsi = syscall::kUserAddressLimit;
    ASSERT_EQ(syscall::HandleSetitimer(frame), -syscall::kEfault);
    frame.rdi = 1;
    ASSERT_EQ(syscall::HandleGetitimer(frame), -syscall::kEinval);
    scheduler.exit_current();
}

cinux::proc::Task* g_alarm_waiter = nullptr;

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_blocked_alarm_body() {
    using namespace cinux;
    auto&       scheduler = proc::Scheduler::self();
    proc::Task& waiter    = *g_alarm_waiter;
    waiter.parent         = scheduler.current();
    waiter.stack_base     = 0x100000;
    scheduler.seat(waiter, false);
    proc::RealTimerValue timer{.value_ns = 100};
    scheduler.real_timer(waiter, 0, timer, true);
    waiter.signals.blocked = syscall::SigSet{1ULL << static_cast<unsigned>(syscall::kSigAlrm - 1)};
    scheduler.on_timer_tick(100);
    ASSERT_EQ(waiter.state, proc::TaskState::kBlocked);
    ASSERT_FALSE(proc::HasPendingSignal(waiter));
    waiter.signals.blocked = {};
    ASSERT_TRUE(proc::HasPendingSignal(waiter));
    waiter.signals.reset();
    waiter.signals.actions[syscall::kSigAlrm - 1].handler = syscall::kSignalIgnore;
    timer.value_ns                                        = 100;
    scheduler.real_timer(waiter, 100, timer, true);
    scheduler.on_timer_tick(200);
    ASSERT_FALSE(proc::HasPendingSignal(waiter));
    ASSERT_EQ(waiter.state, proc::TaskState::kBlocked);
    scheduler.unblock(waiter);
    scheduler.exit_current();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_raw_body() {
    using namespace cinux;
    auto frame                              = plain_frame();
    frame.rdi                               = net::kAfInet;
    frame.rsi                               = net::kSockRaw;
    frame.rdx                               = net::kIpv4ProtocolIcmp;
    proc::Scheduler::self().current()->euid = 1000;
    ASSERT_EQ(syscall::HandleSocket(frame), -syscall::kEperm);
    proc::Scheduler::self().current()->euid = 0;
    const auto kFd                          = syscall::HandleSocket(frame);
    ASSERT_TRUE(kFd >= 0);
    unsigned char address[16]{};
    fill_sockaddr(address, net::kLoopbackAddress, 0);
    unsigned char request[net::kEchoMessageBytes]{};
    net::BuildEchoRequest(request, sizeof(request), 0x3456, 9);
    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kFd);
    frame.rsi = reinterpret_cast<Word>(request);
    frame.rdx = sizeof(request);
    frame.r8  = reinterpret_cast<Word>(address);
    frame.r9  = sizeof(address);
    ASSERT_EQ(syscall::HandleSendto(frame), static_cast<long long>(sizeof(request)));
    net::NetWorld::self().pump_once();
    unsigned char received[128]{};
    struct AddressLength {
        uint32_t bytes  = 16;
        uint32_t canary = 0x12345678;
    } length;
    frame.rsi = reinterpret_cast<Word>(received);
    frame.rdx = sizeof(received);
    frame.r9  = reinterpret_cast<Word>(&length);
    ASSERT_EQ(syscall::HandleRecvfrom(frame), 32);
    ASSERT_EQ(received[20], net::kIcmpEchoRequest);
    ASSERT_EQ(syscall::HandleRecvfrom(frame), 32);
    ASSERT_EQ(received[20], net::kIcmpEchoReply);
    ASSERT_EQ(base::ReadWord16Be(received, 24), 0x3456U);
    ASSERT_EQ(length.bytes, 16U);
    ASSERT_EQ(length.canary, 0x12345678U);
    ASSERT_EQ(base::ReadWord32Be(address, 4), net::kLoopbackAddress);
    ASSERT_TRUE(proc::SendSignal(*proc::Scheduler::self().current(), syscall::kSigInt));
    ASSERT_EQ(syscall::HandleRecvfrom(frame), -syscall::kEintr);
    proc::Scheduler::self().current()->signals.reset();
    frame                   = plain_frame();
    frame.rdi               = net::kAfInet;
    frame.rsi               = static_cast<Word>(net::kSockDgram) | 0x800ULL;
    const auto kNonblocking = syscall::HandleSocket(frame);
    ASSERT_TRUE(kNonblocking >= 0);
    frame.rdi = static_cast<Word>(kNonblocking);
    frame.rsi = reinterpret_cast<Word>(received);
    frame.rdx = sizeof(received);
    ASSERT_EQ(syscall::HandleRecvfrom(frame), -syscall::kEagain);
    proc::Scheduler::self().exit_current();
}

}

TEST("itimer: one-shot, delayed periodic expiry, cancellation and validation") {
    SocketWorld world;
    world.run(run_timer_body);
}

TEST("itimer: a masked or ignored alarm never unlinks a blocked waiter") {
    cinux::proc::Task waiter;
    SocketWorld       world;
    g_alarm_waiter = &waiter;
    world.run(run_blocked_alarm_body);
}

TEST("raw socket: complete IPv4 echoes, four-byte socklen and EINTR") {
    SocketWorld world;
    world.run(run_raw_body);
}

namespace {

void fill_ifreq(unsigned char* request, const char* name) {
    for (unsigned long index = 0; index < 16; ++index) {
        request[index] = 0;
    }
    for (unsigned long index = 0; name[index] != '\0'; ++index) {
        request[index] = static_cast<unsigned char>(name[index]);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) a body walks one story in order
void run_if_ioctl_body() {
    cinux::syscall::SyscallFrame frame = plain_frame();
    frame.rdi                          = cinux::net::kAfInet;
    frame.rsi                          = cinux::net::kSockDgram;
    const long long kFd                = cinux::syscall::HandleSocket(frame);
    ASSERT_TRUE(kFd >= 0);

    unsigned char request[40] = {};
    frame                     = plain_frame();
    frame.rdi                 = static_cast<Word>(kFd);
    frame.rdx                 = reinterpret_cast<Word>(request);

    frame.rsi = 0x8913;
    fill_ifreq(request, "lo");
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    unsigned short flags = 0;
    cinux::base::CopyBytes(&flags, request + 16, 2);
    ASSERT_EQ(flags, 0x49U);

    frame.rsi = 0x8915;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    ASSERT_EQ(cinux::base::ReadWord32Be(request, 20), cinux::net::kLoopbackAddress);
    uint16_t family = 0;
    cinux::base::CopyBytes(&family, request + 16, 2);
    ASSERT_EQ(family, 2U);

    frame.rsi = 0x891b;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    ASSERT_EQ(cinux::base::ReadWord32Be(request, 20), 0xFF000000U);

    frame.rsi = 0x8927;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    uint16_t kind = 0;
    cinux::base::CopyBytes(&kind, request + 16, 2);
    ASSERT_EQ(kind, 772U);

    frame.rsi = 0x8921;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    unsigned int mtu = 0;
    cinux::base::CopyBytes(&mtu, request + 16, 4);
    ASSERT_EQ(mtu, 65536U);

    frame.rsi = 0x8942;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), 0);
    unsigned int qlen = 0;
    cinux::base::CopyBytes(&qlen, request + 16, 4);
    ASSERT_EQ(qlen, 1000U);

    frame.rsi = 0x8917;
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), -cinux::syscall::kEnotty);
    frame.rsi = 0x8913;
    fill_ifreq(request, "eth9");
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), -cinux::syscall::kEnodev);
    fill_ifreq(request, "eth0");
    ASSERT_EQ(cinux::syscall::HandleIoctl(frame), -cinux::syscall::kEnodev);
    cinux::proc::Scheduler::self().exit_current();
}

}  // namespace

TEST("socket ioctl: the interface questions answer the loopback roll") {
    SocketWorld world;
    world.run(run_if_ioctl_body);
}

namespace {
void check_stream_payload(cinux::net::Socket& sender, cinux::net::Socket& receiver) {
    char bytes[8]{};
    ASSERT_EQ(receiver.receive(bytes, 1, nullptr).error(), cinux::base::KernelError::kWouldBlock);
    ASSERT_TRUE(sender.transmit("hello", 5, {}).value() == 5UL);
    ASSERT_EQ(receiver.poll_events(1), 1U);
    ASSERT_TRUE(receiver.receive(bytes, 2, nullptr).value() == 2UL);
    ASSERT_TRUE(receiver.receive(bytes + 2, 6, nullptr).value() == 3UL);
    ASSERT_TRUE(cinux::base::EqualBytes(bytes, "hello", 5));
}

void check_local_binding(cinux::net::Socket& listener, cinux::net::Socket& contender,
                         const cinux::net::SocketAddress& address) {
    ASSERT_TRUE(listener.adopt_local(address).ok());
    ASSERT_EQ(contender.adopt_local(address).error(), cinux::base::KernelError::kAlreadyExists);
    ASSERT_TRUE(listener.begin_listen(2).ok());
}
}

TEST("unix: socketpair keeps stream bytes and a half-close in both directions") {
    cinux::base::Shared<cinux::net::Socket> pair[2];
    ASSERT_TRUE(cinux::net::MakeLocalPair(cinux::net::kSockStream, pair).ok());
    check_stream_payload(*pair[0], *pair[1]);
    const cinux::net::SocketAddress kUnnamed{};
    char                            bytes[8]{};
    ASSERT_TRUE(pair[0]->shutdown_write().ok());
    ASSERT_TRUE(pair[1]->receive(bytes, 1, nullptr).value() == 0UL);
    ASSERT_TRUE(pair[1]->transmit("r", 1, kUnnamed).value() == 1UL);
    ASSERT_TRUE(pair[0]->receive(bytes, 1, nullptr).value() == 1UL);
    pair[0].reset();
    ASSERT_TRUE((pair[1]->poll_events(0) & 0x10U) != 0);
}

TEST("unix: abstract names bind once and accepted endpoints survive their listener") {
    auto                      listener   = cinux::net::MakeLocalSocket(cinux::net::kSockStream);
    const auto                kClient    = cinux::net::MakeLocalSocket(cinux::net::kSockStream);
    const auto                kContender = cinux::net::MakeLocalSocket(cinux::net::kSockStream);
    cinux::net::SocketAddress address{.family = cinux::net::kAfUnix, .name_bytes = 3};
    address.name[1] = 'a';
    address.name[2] = 'b';
    check_local_binding(*listener, *kContender, address);
    ASSERT_TRUE(kClient->adopt_peer(address).ok());
    auto accepted = listener->accept_one();
    ASSERT_TRUE(accepted.ok());
    listener.reset();
    ASSERT_TRUE(kContender->adopt_local(address).ok());
    ASSERT_TRUE(kClient->transmit("a", 1, {}).value() == 1UL);
    char byte{};
    ASSERT_TRUE(accepted.value()->receive(&byte, 1, nullptr).value() == 1UL);
    ASSERT_EQ(byte, 'a');
}

TEST("unix: datagram truncation drops one message and preserves the next boundary") {
    cinux::base::Shared<cinux::net::Socket> pair[2];
    ASSERT_TRUE(cinux::net::MakeLocalPair(cinux::net::kSockDgram, pair).ok());
    ASSERT_TRUE(pair[0]->transmit("abc", 3, {}).value() == 3UL);
    ASSERT_TRUE(pair[0]->transmit("z", 1, {}).value() == 1UL);
    char byte{};
    ASSERT_TRUE(pair[1]->receive(&byte, 1, nullptr).value() == 1UL);
    ASSERT_EQ(byte, 'a');
    ASSERT_TRUE(pair[1]->receive(&byte, 1, nullptr).value() == 1UL);
    ASSERT_EQ(byte, 'z');
    ASSERT_EQ(pair[1]->receive(&byte, 1, nullptr).error(), cinux::base::KernelError::kWouldBlock);
}

TEST("unix race: two host threads exchange ordered stream bytes under the real socket lock") {
    cinux::base::Shared<cinux::net::Socket> pair[2];
    ASSERT_TRUE(cinux::net::MakeLocalPair(cinux::net::kSockStream, pair).ok());
    std::atomic<bool> failed{false};
    std::thread       writer([&] {
        cinux::arch::per_cpu::PerCpu::self().cpu_id = 1;
        for (unsigned long index = 0; index < 4096; ++index) {
            const auto kByte = static_cast<unsigned char>(index);
            const auto kSent = pair[0]->transmit(&kByte, 1, {});
            if (!kSent.ok() || kSent.value() != 1) {
                failed.store(true);
                return;
            }
        }
    });
    std::thread       reader([&] {
        cinux::arch::per_cpu::PerCpu::self().cpu_id = 2;
        for (unsigned long index = 0; index < 4096;) {
            unsigned char byte{};
            const auto    kRead = pair[1]->receive(&byte, 1, nullptr);
            if (!kRead.ok() && kRead.error() == cinux::base::KernelError::kWouldBlock) {
                std::this_thread::yield();
                continue;
            }
            if (!kRead.ok() || kRead.value() != 1 || byte != static_cast<unsigned char>(index)) {
                failed.store(true);
                return;
            }
            ++index;
        }
    });
    writer.join();
    reader.join();
    ASSERT_FALSE(failed.load());
}

int main() {
    return cinux::test::RunAll();
}
