/**
 * @file kernel/net/net_init.hpp
 * @brief Production network-subsystem entry points: bring the stack up at boot
 *        and issue an ICMP echo ("ping") from kernel context.
 *
 * DECLARATIONS only (this header lives in kernel/net/ and pulls in no driver /
 * DMA / arch-irq header -- the four decoupling greps hold).  The IMPLEMENTATION
 * that names E1000Controller lives in kernel/drivers/net/net_init.cpp (the one
 * wiring point that sees both the driver and the L3 stack); a no-op stub in
 * net_stub.cpp covers a CINUX_NET=OFF build (§14 -- main.cpp's call site has no
 * #ifdef).
 *
 * ping() drives its OWN sti/hlt+poll loop for the duration of the echo, so no
 * persistent net thread is needed for "ping out" (the syscall is the poll
 * driver while it runs).  Passive RX (answering pings TO us) needs a resident
 * poll driver -- a follow-up, not this batch.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/net_types.hpp"

namespace cinux::net {

/// @brief Result of a single ping() echo.
struct PingResult {
    bool     got_reply = false;  ///< an ICMP echo reply was received
    uint16_t id        = 0;      ///< reply echo id (matches the request)
    uint16_t seq       = 0;      ///< reply echo seq (matches the request)
};

/// @brief Bring the L3 stack up at boot: build the adapter over the primary NIC
///        (the e1000 singleton published by cinux::drivers::net::init()), the
///        ARP / IPv4 / ICMP modules + NetStack, register protocols, and attach.
///        No-op (graceful) if no NIC is present.  Call once after drivers::net::init().
void init();

/// @brief Send one ICMP echo request to @p dst and poll (sti/hlt) until the
///        reply lands or the budget is exhausted.  Drives NetStack::poll() for
///        the duration -- the syscall that calls this is the poll driver.
/// @return PingResult (got_reply set on success); Error::NotImplemented if the
///         stack is not up (no NIC / init() not called).
cinux::lib::ErrorOr<PingResult> ping(Ipv4Addr dst, uint16_t id, uint16_t seq);

}  // namespace cinux::net
