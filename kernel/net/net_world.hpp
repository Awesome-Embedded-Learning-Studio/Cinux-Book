/**
 * @file    net_world.hpp
 * @brief   The production wiring: one stack, its modules, one pump.
 *
 * Everything the protocol family needs to run for real, owned by one
 * singleton: the loopback port, the stack, the IPv4 and ICMP modules,
 * and the address book. init() wires them together — ports to stack,
 * seats to protocols, sinks to send paths — and start_pump() seats the
 * one resident thread that keeps the whole world draining. The pump is
 * the injectable pump of the station's story: pump_once() is the whole
 * engine, the thread merely calls it and yields, and a test may drive
 * it by hand instead. Why a syscall-open world still wants a resident
 * thread is the halt-semantics lesson the chapter tells.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cinux/singleton.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/arp.hpp"
#include "kernel/net/icmp.hpp"
#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_stack.hpp"

namespace cinux::net {

/**
 * @brief   The one production wiring of the protocol family.
 * @note    A thread that sends races the pump's counters and nothing
 *          else: the drain loop itself is the pump's alone, so packet
 *          order is single-threaded even on two CPUs. The counters are
 *          advisory scoreboards, not synchronization.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class NetWorld : public cinux::base::Singleton<NetWorld> {
    friend class cinux::base::Singleton<NetWorld>;

public:
    /**
     * @brief         Wires the loopback world together.
     * @return        None
     * @note          Idempotent: re-running returns the world to a clean
     *                slate with the pump, if seated, still draining it.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void init();

    /**
     * @brief         Seats the resident pump thread, once.
     * @return        True when a pump runs; false only if the build
     *                failed, which the wiring treats as fatal.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    bool start_pump();

    /**
     * @brief         Joins a real wire to the world and reroutes sends.
     *
     * @param[in,out] wire  The port of a framing device the boot found.
     * @return        None
     * @note          Loopback-bound datagrams keep their loopback path;
     *                everything else routes through the wire, resolving
     *                hardware addresses from the cache and sending a
     *                request when the cache cannot answer. The kernel's address
     *                becomes the user-network one the wire serves.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void attach_wire(driver::net::NetPort& wire);

    /**
     * @brief         Sends echo requests and counts the answers.
     *
     * @param[in]     destination  Address to ask.
     * @param[in]     attempts     How many requests to send.
     * @return        How many echo replies came back.
     * @note          The demo face: each unanswered round may still be
     *                doing the address-resolution work the next round
     *                needs, so misses are spent, not wasted. Waits by
     *                yielding when the pump runs, by draining by hand
     *                when it does not.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    unsigned long ping_sweep(uint32_t destination, unsigned long attempts);

    /**
     * @brief         Drains the stack once under the frame budget.
     * @return        None
     * @note          The injectable pump: the thread's whole body, and
     *                        a test's handle when it prefers to drive.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void pump_once();

    /**
     * @brief         The wired stack.
     * @return        Reference to the owned stack.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] NetStack& stack();

    /**
     * @brief         The wired IPv4 module.
     * @return        Reference to the owned module.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] Ipv4Module& ipv4();

    /**
     * @brief         The wired ICMP module.
     * @return        Reference to the owned module.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] IcmpModule& icmp();

    /**
     * @brief         The wired address book.
     * @return        Reference to the owned cache.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] ArpCache& arp();

    /**
     * @brief         Whether a pump thread is seated.
     * @return        True once start_pump() succeeded.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] bool pump_running() const;

private:
    NetWorld() = default;

    static void pump_entry();
    static void route_thunk(void* context, const OutboundDatagram& datagram);
    void        route_send(const OutboundDatagram& datagram);
    void        wait_for_answers(unsigned long& seen);

    struct ArpEntry {
        NetWorld* owner = nullptr;

        void handle(const unsigned char* bytes, size_t length) const;
    };

    driver::net::NetPort* wire_port_   = nullptr;
    ArpEntry              arp_entry_   = {};
    uint32_t              own_address_ = 0;
    driver::net::NetPort  loopback_port_;
    NetStack              stack_        = {};
    Ipv4Module            ipv4_         = {};
    IcmpModule            icmp_         = {};
    ArpCache              arp_          = {};
    bool                  pump_running_ = false;
};

}  // namespace cinux::net
