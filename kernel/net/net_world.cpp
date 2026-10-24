#include "kernel/net/net_world.hpp"

#include <stddef.h>
#include <stdint.h>

#include "cinux/assert.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/arp.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_clock.hpp"

namespace cinux::net {

namespace {

constexpr unsigned char kPumpPriority = 200;

void send_to_loopback(void* context, const OutboundDatagram& datagram) {
    auto&                         device    = *static_cast<driver::net::Loopback*>(context);
    const driver::net::MacAddress kNobody   = {};
    const driver::net::EtherType  kIpv4Type = {kEtherTypeIpv4};
    base::safety::Check(device.send_l3(kNobody, kIpv4Type, datagram.payload, datagram.length),
                        "loopback queue refused a send");
}

void send_via_ipv4(void* context, const OutboundDatagram& datagram) {
    static_cast<Ipv4Module*>(context)->send(datagram);
}

}  // namespace

void NetWorld::init() {
    driver::net::Loopback::self().init();
    loopback_port_.adopt(driver::net::Loopback::self());
    own_address_ = kLoopbackAddress;

    stack_.init();
    base::safety::Check(stack_.attach_port(loopback_port_), "port table refused the loopback");

    const DatagramSink kToWire = {
        .context = &driver::net::Loopback::self(),
        .send    = send_to_loopback,
    };
    ipv4_.init(kToWire, kLoopbackAddress);
    ipv4_.observe(&raw_, [](void* context, const unsigned char* bytes, size_t length,
                            const Ipv4View& header) {
        static_cast<RawModule*>(context)->handle(bytes, length, header);
    });
    const DatagramSink kViaIpv4 = {.context = &ipv4_, .send = send_via_ipv4};
    icmp_.init(kViaIpv4);
    udp_.init(kViaIpv4, kLoopbackAddress);
    arp_.init();
    arp_entry_ = {this};

    L4Seat icmp_seat;
    icmp_seat.adopt(icmp_);
    base::safety::Check(ipv4_.seat_protocol(kIpv4ProtocolIcmp, icmp_seat),
                        "layer-four table refused icmp");
    L4Seat udp_seat;
    udp_seat.adopt(udp_);
    base::safety::Check(ipv4_.seat_protocol(kIpv4ProtocolUdp, udp_seat),
                        "layer-four table refused udp");
    ProtocolSeat ipv4_seat;
    ipv4_seat.adopt(ipv4_);
    base::safety::Check(stack_.seat_protocol(kEtherTypeIpv4, ipv4_seat),
                        "protocol table refused ipv4");
    ProtocolSeat arp_seat;
    arp_seat.adopt(arp_entry_);
    base::safety::Check(stack_.seat_protocol(kEtherTypeArp, arp_seat),
                        "protocol table refused arp");
}

bool NetWorld::start_pump() {
    if (pump_running_) {
        return true;
    }
    auto* const kPump = proc::TaskBuilder{}
                            .set_entry(pump_entry)
                            .set_name("net_poll")
                            .set_priority(kPumpPriority)
                            .build();
    base::safety::Check(kPump != nullptr, "net pump task failed to build");
    proc::Scheduler::self().seat(*kPump, false);
    proc::Scheduler::self().unblock(*kPump);
    pump_running_ = true;
    return true;
}

void NetWorld::pump_once() {
    const proc::PreemptGuard kPin;
    stack_.poll(kPollBudgetFrames);
}

void NetWorld::attach_wire(driver::net::NetPort& wire) {
    wire_port_                 = &wire;
    own_address_               = kUserNetworkSelf;
    const DatagramSink kRouted = {.context = this, .send = NetWorld::route_thunk};
    ipv4_.aim(kRouted, kUserNetworkSelf);
    const DatagramSink kViaIpv4 = {.context = &ipv4_, .send = send_via_ipv4};
    udp_.aim(kViaIpv4, kUserNetworkSelf);
    base::safety::Check(stack_.attach_port(wire), "port table refused the wire");
}

void NetWorld::route_thunk(void* context, const OutboundDatagram& datagram) {
    static_cast<NetWorld*>(context)->route_send(datagram);
}

void NetWorld::route_send(const OutboundDatagram& datagram) {
    const auto kRoute = route_gate_.guard();
    if ((datagram.destination & kLoopbackNetMask) == kLoopbackNet || wire_port_ == nullptr) {
        const driver::net::MacAddress kNobody   = {};
        const driver::net::EtherType  kIpv4Type = {kEtherTypeIpv4};
        base::safety::Check(driver::net::Loopback::self().send_l3(
                                kNobody, kIpv4Type, datagram.payload, datagram.length),
                            "loopback queue refused a send");
        return;
    }

    const driver::net::MacAddress* next = arp_.lookup(datagram.destination);
    if (next == nullptr) {
        unsigned char           request[kArpPacketBytes] = {};
        driver::net::MacAddress everyone                 = {};
        for (unsigned long index = 0; index < driver::net::kMacAddressBytes; ++index) {
            everyone.bytes[index] = driver::net::kBroadcastMacBytes[index];
        }
        ArpCache::build_request(request, wire_port_->mac(), own_address_, datagram.destination);
        base::safety::Check(wire_port_->send_l3(everyone, driver::net::EtherType{kEtherTypeArp},
                                                request, kArpPacketBytes),
                            "wire refused the address request");
        return;
    }
    base::safety::Check(wire_port_->send_l3(*next, driver::net::EtherType{kEtherTypeIpv4},
                                            datagram.payload, datagram.length),
                        "wire refused the datagram");
}

void NetWorld::wait_for_answers(unsigned long& seen) {
    const unsigned long long kDeadline = syscall::MonotonicNanoseconds() + kAnswerWaitNanoseconds;
    while (syscall::MonotonicNanoseconds() < kDeadline) {
        if (pump_running_) {
            proc::Scheduler::self().yield();
        } else {
            pump_once();
            arch::EnableIrqAndHalt();
        }
        if (icmp_.stats().replies_seen != seen) {
            seen = icmp_.stats().replies_seen;
            return;
        }
    }
}

unsigned long NetWorld::ping_sweep(uint32_t destination, unsigned long attempts) {
    const unsigned long kBefore                    = icmp_.stats().replies_seen;
    unsigned long       seen                       = kBefore;
    unsigned char       request[kEchoMessageBytes] = {};
    for (unsigned long attempt = 0; attempt < attempts; ++attempt) {
        BuildEchoRequest(request, sizeof(request), 0x1234, static_cast<uint16_t>(attempt + 1));
        const bool kSent = ipv4_.send({
            .destination = destination,
            .protocol    = kIpv4ProtocolIcmp,
            .payload     = request,
            .length      = sizeof(request),
        });
        base::safety::Check(kSent, "the stack refused an echo request");
        wait_for_answers(seen);
        if (icmp_.stats().replies_seen != seen) {
            print::Println("[net] echo answered: id=0x%x seq=%u", icmp_.stats().last_identifier,
                           icmp_.stats().last_sequence);
        }
    }
    return icmp_.stats().replies_seen - kBefore;
}

NetStack& NetWorld::stack() {
    return stack_;
}

Ipv4Module& NetWorld::ipv4() {
    return ipv4_;
}

IcmpModule& NetWorld::icmp() {
    return icmp_;
}

UdpModule& NetWorld::udp() {
    return udp_;
}

RawModule& NetWorld::raw() {
    return raw_;
}

ArpCache& NetWorld::arp() {
    return arp_;
}

bool NetWorld::pump_running() const {
    return pump_running_;
}

bool NetWorld::wired() const {
    return wire_port_ != nullptr;
}

driver::net::NetPort* NetWorld::wire() const {
    return wire_port_;
}

void NetWorld::pump_entry() {
    for (;;) {
        self().pump_once();
        proc::Scheduler::self().yield();
    }
}

void NetWorld::ArpEntry::handle(const unsigned char* bytes, size_t length) const {
    const auto kRoute = owner->route_gate_.guard();
    owner->arp_.observe(bytes, length);
}

}  // namespace cinux::net
