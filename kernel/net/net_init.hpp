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
 * RX is driven by a resident net_poll kernel thread (start_poll_driver()) that
 * polls NetStack + sti/hlt's to keep QEMU's main loop running.  ping() drives
 * an INJECTABLE pump: the production default yields (no sti/hlt inside the
 * syscall -- sti mid-syscall re-enables the timer IRQ and corrupts the syscall
 * trap frame -> #DF), while tests inject rx_pump_sti_hlt (or a mock).  This
 * "driven method" seam is what lets the #DF-safe production path coexist with
 * the test kernel's non-preemptive inline-sti/hlt RX driver.
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

/// RX pump: drive net receive one step; return true once the ICMP reply has
/// landed.  Inject a mock (or rx_pump_sti_hlt) for tests; production uses the
/// default yield-pump (see ping()).
using RxPump = bool (*)();

/// @brief Send one ICMP echo request to @p dst and drive @p pump until the
///        reply lands (drained by the net_poll kthread) or the budget is
///        exhausted (-ETIMEDOUT).  @p pump defaults to the production
///        yield-pump (NO sti/hlt inside the syscall -- the #DF hazard);
///        tests pass rx_pump_sti_hlt or a mock.
/// @return PingResult (got_reply set on success); Error::NotImplemented if the
///         stack is not up (no NIC / init() not called).
cinux::lib::ErrorOr<PingResult> ping(Ipv4Addr dst, uint16_t id, uint16_t seq,
                                     RxPump pump = nullptr);

/// @brief Legacy inline RX pump (poll + sti/hlt) for tests exercising real
///        SLIRP delivery.  Safe in the test kernel (its timer handler does not
///        tick); NEVER call from a production syscall (sti there is the #DF).
bool rx_pump_sti_hlt();

/// @brief Spawn the background net RX poll-driver kernel thread.  Call ONCE
///        after Scheduler::init(); a no-op if no NIC was found at init().
void start_poll_driver();

/// @brief Override the default RX pump used when ping() is called with no pump
///        (e.g. via sys_ping).  Test kernels set this to rx_pump_sti_hlt (no
///        net_poll kthread runs there); production leaves the yield default.
///        Pass nullptr to restore the production default.
void set_default_rx_pump(RxPump pump);

class Socket;  // forward -- see kernel/net/socket.hpp (F7-M6)

/// @brief Allocate a Socket wired to the production stack (F7-M6).  The factory
///        is the ONE bridge between the socket layer (kernel/net/) and the L4
///        modules instantiated here: sys_socket calls it, and it returns a
///        UdpSocket (SOCK_DGRAM) / TcpSocket (SOCK_STREAM) bound to g_udp/g_tcp
///        + the route resolver.  Returns nullptr if the stack is not up (no NIC)
///        or the domain/type is unsupported (sys_socket maps that to -EPROTONOSUPPORT).
///        Implemented in kernel/drivers/net/net_init.cpp (the composition root).
Socket* create_socket(int domain, int type);

}  // namespace cinux::net
