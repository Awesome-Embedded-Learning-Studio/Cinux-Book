/**
 * @file kernel/drivers/net/net_init.cpp
 * @brief Production net wiring: cinux::net::init() + cinux::net::ping().
 *
 * The ONE composition root that sees BOTH the e1000 driver and the L3 stack:
 * builds the E1000NetDevice adapter over the singleton published by
 * cinux::drivers::net::init(), the ARP/IPv4/ICMP modules + NetStack, and
 * attaches them.  ping() issues an ICMP echo + drives a sti/hlt+poll loop (the
 * syscall calling it IS the poll driver for the duration -- no resident net
 * thread needed for "ping out").
 *
 * Lives in drivers/net/ (NOT kernel/net/) because it names E1000Controller;
 * the public API is declared in kernel/net/net_init.hpp, which stays decoupled.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/net_init.hpp"

#include <cstdint>

#include "kernel/arch/x86_64/irq.hpp"  // sti/hlt for the SLIRP delivery loop
#include "kernel/drivers/net/e1000.hpp"
#include "kernel/drivers/net/e1000_net_device.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/net/arp.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_stack.hpp"

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
Ipv4Module*     g_ipv4    = nullptr;
E1000NetDevice* g_adapter = nullptr;
bool            g_ready   = false;

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

    g_ready = true;
    cinux::lib::kprintf("[net] L3 stack up: %u.%u.%u.%u -> gw %u.%u.%u.%u\n", cfg.local.oct[0],
                        cfg.local.oct[1], cfg.local.oct[2], cfg.local.oct[3], cfg.gateway.oct[0],
                        cfg.gateway.oct[1], cfg.gateway.oct[2], cfg.gateway.oct[3]);
}

cinux::lib::ErrorOr<PingResult> ping(Ipv4Addr dst, uint16_t id, uint16_t seq) {
    if (!g_ready) {
        return cinux::lib::Error::NotImplemented;
    }
    g_icmp.reset();

    // Send + sti/hlt + poll loop.  Iter 1: ARP resolve miss -> ARP request sent,
    // IP deferred.  Poll (sti+hlt lets QEMU's main loop run + a timer IRQ wakes
    // us) -> SLIRP's ARP reply caches the gateway MAC.  Iter 2+: ARP hit ->
    // ICMP echo out -> poll -> echo reply lands.  Same timing the F5-M6 批b-fix
    // test proved; production runs IF=1 so the loop's sti/hlt is woken by the
    // scheduling tick (no separate trap-loop hack).
    for (uint32_t i = 0; i < 4000 && g_icmp.reply_count() == 0; ++i) {
        (void)g_icmp.send_echo_request(*g_adapter, dst, id, seq, *g_ipv4, g_stack);
        for (uint32_t j = 0; j < 4; ++j) {
            g_stack.poll();
            if (g_icmp.reply_count() > 0) {
                break;
            }
            cinux::arch::irq_enable();
            cinux::arch::hlt();
            cinux::arch::irq_disable();
        }
    }

    PingResult r;
    r.got_reply = g_icmp.reply_count() >= 1;
    r.id        = g_icmp.last_reply_id();
    r.seq       = g_icmp.last_reply_seq();
    return r;
}

}  // namespace cinux::net
