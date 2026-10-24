#include <stddef.h>
#include <stdint.h>

#include "../framework/framework.hpp"
#include "cinux/assert.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/net_world.hpp"
#include "kernel/net/socket.hpp"
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
unsigned long long g_test_monotonic_ns = 0;

// NOLINTNEXTLINE(misc-use-internal-linkage) the link stub must stay external
unsigned long long MonotonicNanoseconds() {
    return g_test_monotonic_ns;
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

}  // namespace

TEST("socket adapter: bind seats a port and getsockname answers it") {
    Wiring                          wiring;
    cinux::net::UdpSocket           socket(wiring.udp);
    const cinux::net::SocketAddress kAny{};

    ASSERT_TRUE(socket.adopt_local(kAny).ok());
    cinux::net::SocketAddress seated{};
    ASSERT_TRUE(socket.report_local(seated).ok());
    ASSERT_EQ(seated.port, cinux::net::kUdpEphemeralFirst);
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
    ASSERT_EQ(cinux::syscall::HandleSocketpair(frame), -cinux::syscall::kEopnotsupp);

    frame                    = plain_frame();
    frame.rdi                = cinux::net::kAfInet;
    frame.rsi                = 1;
    const long long kBadType = cinux::syscall::HandleSocket(frame);
    ASSERT_EQ(kBadType, -cinux::syscall::kEsocktnosupport);
    frame.rsi = cinux::net::kSockDgram;
    frame.rdx = 6;
    ASSERT_EQ(cinux::syscall::HandleSocket(frame), -cinux::syscall::kEprotonosupport);

    frame.rdx           = 0;
    const long long kFd = cinux::syscall::HandleSocket(frame);
    ASSERT_TRUE(kFd >= 0);

    frame     = plain_frame();
    frame.rdi = static_cast<Word>(kFd);
    ASSERT_EQ(cinux::syscall::HandleListen(frame), -cinux::syscall::kEopnotsupp);
    ASSERT_EQ(cinux::syscall::HandleAccept(frame), -cinux::syscall::kEopnotsupp);
    ASSERT_EQ(cinux::syscall::HandleAccept4(frame), -cinux::syscall::kEopnotsupp);
    ASSERT_EQ(cinux::syscall::HandleShutdown(frame), -cinux::syscall::kEnotconn);

    frame.rsi           = cinux::syscall::kSolSocket;
    frame.rdx           = cinux::syscall::kSoType;
    unsigned int answer = 0;
    uint32_t     length = sizeof(answer);
    frame.r10           = reinterpret_cast<Word>(&answer);
    frame.r8            = reinterpret_cast<Word>(&length);
    ASSERT_EQ(cinux::syscall::HandleGetsockopt(frame), 0);
    ASSERT_EQ(answer, static_cast<unsigned int>(cinux::net::kSockDgram));

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

int main() {
    return cinux::test::RunAll();
}
