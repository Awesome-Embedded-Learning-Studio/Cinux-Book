/**
 * @file kernel/drivers/net/net_init.cpp
 * @brief Production net wiring: cinux::net::init() + ping() + poll driver.
 *
 * The ONE composition root that sees BOTH the e1000 driver and the L3 stack:
 * builds the E1000NetDevice adapter over the singleton published by
 * cinux::drivers::net::init(), the ARP/IPv4/ICMP modules + NetStack, attaches
 * them, and spawns a resident net_poll kthread.
 *
 * ping() drives RX through an INJECTABLE pump (the "driven method"):
 *   - production default = pump_yield: YIELD (no sti/hlt inside the syscall -- a
 *     resident net_poll kthread owns sti/hlt + NetStack::poll() and drains the
 *     reply).  sti inside sys_ping re-enables the LAPIC-timer IRQ, which fires
 *     schedule() with the syscall trap frame live on the per-CPU stack and
 *     corrupts the saved user RIP/RSP -> #DF on sysretq (the F7 shell-ping
 *     crash).  yield() switches via Task::ctx, the same safe path the TTY
 *     blocking read uses.
 *   - tests inject rx_pump_sti_hlt (the legacy inline sti/hlt+poll) or a mock;
 *     the test kernel's timer handler does not tick, so sti there is safe.
 *
 * Lives in drivers/net/ (NOT kernel/net/) because it names E1000Controller; the
 * public API is declared in kernel/net/net_init.hpp, which stays decoupled.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/net_init.hpp"

#include <cstdint>

#include "kernel/arch/x86_64/irq.hpp"  // sti/hlt for the SLIRP poll loop
#include "kernel/drivers/net/e1000.hpp"
#include "kernel/drivers/net/e1000_net_device.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/net/arp.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/loopback_device.hpp"
#include "kernel/net/net_stack.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/net/tcp.hpp"
#include "kernel/net/tcp_socket.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/net/udp_socket.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task_builder.hpp"

using cinux::drivers::net::E1000Controller;
using cinux::drivers::net::E1000NetDevice;

namespace cinux::net {

namespace {

// Default-constructible modules + stack live as file-scope statics.  Ipv4Module
// (ctor args: icmp + arp) and E1000NetDevice (ctor arg: the controller) are
// constructed once inside init() (function-local statics + cached pointers) so
// their dependencies exist first.
ArpModule       g_arp;
IcmpModule      g_icmp;
NetStack        g_stack;
UdpModule       g_udp;     // proto 17 -- registered into the L4 table in init()
TcpModule       g_tcp;     // proto  6 -- registered into the L4 table in init()
LoopbackDevice  g_lo;      // 127.0.0.1 software NIC (~12 KB; static, never stack)
Ipv4Module*     g_ipv4    = nullptr;
E1000NetDevice* g_adapter = nullptr;
bool            g_ready   = false;

// Background net RX poll driver kthread body.  Drains the e1000 ring via
// NetStack::poll() and sti/hlt's to keep QEMU's main loop running so SLIRP
// injects ARP/ICMP replies.  This is a kernel THREAD, not a syscall: being
// preempted mid-loop is safe (its context is saved via Task::ctx, not a syscall
// trap frame), so it can sti/hlt freely -- unlike ping(), which must NOT.
// Spawned once by start_poll_driver().
void net_poll_entry() {
    while (true) {
        g_stack.poll();
        cinux::arch::irq_enable();
        cinux::arch::hlt();
        cinux::arch::irq_disable();
    }
}

// Production default RX pump: yield (let the net_poll kthread drain + advance
// QEMU) then report whether the ICMP reply has landed.  NO sti/hlt here -- this
// runs inside sys_ping; sti would re-enable the LAPIC-timer IRQ and corrupt the
// syscall trap frame (#DF).  yield() context-switches via Task::ctx.
bool pump_yield() {
    cinux::proc::Scheduler::yield();
    return g_icmp.reply_count() > 0;
}

// Default pump when ping() is called with no pump argument (e.g. via sys_ping).
// Production = pump_yield; a test kernel overrides it via set_default_rx_pump()
// (it has no net_poll kthread, so the yield pump would never see a reply).
RxPump g_default_pump = pump_yield;

}  // namespace

void init() {
    if (g_ready) {
        return;  // already wired (idempotent)
    }
    if (!E1000Controller::has_controller()) {
        cinux::lib::kprintf("[net] no NIC -- L3 stack not initialised\n");
        return;
    }

    // Construct the two arg-taking singletons once; cache pointers for ping().
    static Ipv4Module     ipv4(g_icmp, &g_arp);
    static E1000NetDevice adapter(E1000Controller::instance());
    g_ipv4    = &ipv4;
    g_adapter = &adapter;

    g_stack.add_protocol(kEtherTypeArp, g_arp);
    g_stack.add_protocol(kEtherTypeIpv4, *g_ipv4);

    InDevice cfg{};
    EthAddr  mac{};
    g_adapter->mac(mac);
    cfg.hw      = mac;
    cfg.local   = kSlirpGuest;    // 10.0.2.15
    cfg.gateway = kSlirpGateway;  // 10.0.2.2 (SLIRP answers ARP + ICMP echo)
    g_stack.attach(*g_adapter, cfg);

    // Register UDP (proto 17) + TCP (proto 6) L4 handlers so inbound segments
    // reach the modules' handle() instead of a null-slot drop.  ICMP is already
    // auto-registered by the Ipv4Module ctor; kMaxL4=4 leaves room for all three.
    g_ipv4->add_l4(kIpProtoUdp, g_udp);
    g_ipv4->add_l4(kIpProtoTcp, g_tcp);

    // Loopback (127.0.0.1): the deterministic L3 path sockets ride for local
    // traffic + tests.  No L2 / no gateway -- send_l3 queues and poll() loops it
    // back on the next round.  kMaxDevs=2 fits the e1000 adapter + loopback.
    InDevice lo_cfg{};
    lo_cfg.local = kLoopbackAddr;
    g_stack.attach(g_lo, lo_cfg);

    g_ready = true;
    cinux::lib::kprintf("[net] L3 stack up: %u.%u.%u.%u -> gw %u.%u.%u.%u\n", cfg.local.oct[0],
                        cfg.local.oct[1], cfg.local.oct[2], cfg.local.oct[3], cfg.gateway.oct[0],
                        cfg.gateway.oct[1], cfg.gateway.oct[2], cfg.gateway.oct[3]);
    cinux::lib::kprintf("[net] loopback 127.0.0.1 attached; L4 registered: ICMP+UDP+TCP\n");
}

cinux::lib::ErrorOr<PingResult> ping(Ipv4Addr dst, uint16_t id, uint16_t seq, RxPump pump) {
    if (!g_ready) {
        return cinux::lib::Error::NotImplemented;
    }
    if (pump == nullptr) {
        pump = g_default_pump;  // production yield default; tests override it
    }
    g_icmp.reset();

    // Send + drive the (injected) pump until the reply lands or the budget runs
    // out (-ETIMEDOUT).  Iter 1: ARP resolve miss -> ARP request; iter 2+: ARP
    // hit -> ICMP echo.  Budget bounds the wait (no kernel timer-wake facility
    // exists yet; a real ms deadline is a future milestone).
    constexpr uint32_t kRounds        = 200;  // re-sends (ARP then ICMP)
    constexpr uint32_t kPumpsPerRound = 4;    // RX drains per send
    for (uint32_t s = 0; s < kRounds && g_icmp.reply_count() == 0; ++s) {
        (void)g_icmp.send_echo_request(*g_adapter, dst, id, seq, *g_ipv4, g_stack);
        for (uint32_t k = 0; k < kPumpsPerRound; ++k) {
            if (pump()) {
                break;
            }
        }
    }

    PingResult r;
    r.got_reply = g_icmp.reply_count() >= 1;
    r.id        = g_icmp.last_reply_id();
    r.seq       = g_icmp.last_reply_seq();
    return r;
}

bool rx_pump_sti_hlt() {
    // Legacy inline RX pump (poll + sti/hlt): the proven test-kernel RX driver.
    // Exposed so tests exercising REAL SLIRP delivery can drive ping() without
    // the production yield pump + kthread.  The test kernel's LAPIC-timer
    // handler deliberately does NOT call Scheduler::tick(), so sti here cannot
    // preempt -- safe in tests, but NEVER use this from a syscall in production
    // (that is the #DF hazard pump_yield exists to avoid).
    g_stack.poll();
    if (g_icmp.reply_count() > 0) {
        return true;
    }
    cinux::arch::irq_enable();
    cinux::arch::hlt();
    cinux::arch::irq_disable();
    return g_icmp.reply_count() > 0;
}

void start_poll_driver() {
    if (!g_ready) {
        return;  // no NIC -> nothing to poll
    }
    auto* t = cinux::proc::TaskBuilder().set_entry(net_poll_entry).set_name("net_poll").build();
    if (t != nullptr) {
        cinux::proc::Scheduler::add_task(t);
        cinux::lib::kprintf("[net] poll driver started\n");
    }
}

void set_default_rx_pump(RxPump pump) {
    g_default_pump = (pump != nullptr) ? pump : pump_yield;
}

// Route resolver for sockets: 127/8 -> loopback, else -> the e1000 adapter.
// g_ready guarantees g_adapter is non-null on the non-loopback path.
static NetDevice& dev_for(Ipv4Addr dst) {
    if (dst.oct[0] == 127) {
        return g_lo;
    }
    return *g_adapter;
}

Socket* create_socket(int domain, int type) {
    if (domain != kAfInet) {
        return nullptr;
    }
    // SOCK_DGRAM/SOCK_STREAM with the production stack up: real adapters wired to
    // g_udp/g_tcp. The test kernel (no production net) falls through to a bare
    // stub Socket so the fd machinery stays exercisable there (tests build their
    // own loopback stack + construct the adapters directly).
    if (g_ready) {
        if (type == kSockDgram) {
            return new UdpSocket(g_udp, *g_ipv4, g_stack, dev_for);
        }
        if (type == kSockStream) {
            return new TcpSocket(g_tcp, *g_ipv4, g_stack, dev_for);
        }
    }
    return new Socket(domain, type);
}

}  // namespace cinux::net
