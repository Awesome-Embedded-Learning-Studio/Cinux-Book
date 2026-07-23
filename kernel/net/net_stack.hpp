/**
 * @file kernel/net/net_stack.hpp
 * @brief NetStack -- the front-end: device list + ethertype dispatch table +
 *        RX pump.  The ONLY object L3 protocols see; they NEVER see a driver.
 *
 * Holds a SMALL FIXED DEVICE LIST (FOLD-B: coexistence from day one -- a 2nd
 * NIC attaches instead of retrofitting a single-device stack) and a small
 * ethertype->ProtocolHandler table.  poll() drains one frame from each
 * attached device, parses L2, dispatches by ethertype, and recycles the buffer
 * via a scope guard on every exit path.  Exposed as an instance (NOT a process
 * global) so multi-NIC and tests can spin their own.
 *
 * The QEMU "sti+hlt so the main loop runs SLIRP replies" timing hack lives in
 * the CALLER of poll() (test sync loop / production tick / future ISR
 * bottom-half) -- NEVER inside this class.  kernel/net/ includes no irq header.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/net_device.hpp"
#include "kernel/net/net_types.hpp"

namespace cinux::net {

class ProtocolHandler;  // forward -- see protocol_handler.hpp

/// @brief Per-device IP configuration (Linux in_device/ip_ifaddr, one address).
///
/// Carried on the device handle (FOLD-B: per-device, not a global tuple).  The
/// MAC is read from the device at attach() time and cached here so ARP sources
/// it without re-querying.
struct InDevice {
    EthAddr  hw{};       ///< our MAC (meaningful only if has_ethernet)
    Ipv4Addr local{};    ///< our address (10.0.2.15 on SLIRP, 127.0.0.1 loopback)
    Ipv4Addr gateway{};  ///< default gateway (10.0.2.2 on SLIRP)
};

class NetStack {
public:
    static constexpr uint32_t kMaxDevs   = 2;  ///< coexistence from day one (FOLD-B)
    static constexpr uint32_t kMaxProtos = 8;

    /// @brief Register a NIC with its IP config.  Idempotent per device pointer
    ///        (re-attach updates the config).  Returns false if the device list
    ///        is full.
    bool attach(NetDevice& dev, const InDevice& cfg);

    /// @brief Register a protocol handler for an ethertype (HOST order).  Called
    ///        at boot before RX.  Re-registering an ethertype replaces the handler.
    void add_protocol(uint16_t ethertype, ProtocolHandler& h);

    /// @brief Drain ONE frame from EACH attached device and dispatch by
    ///        ethertype.  The buffer is recycled via scope guard on every exit
    ///        path (handle / drop / runt).  Caller owns the QEMU timing.
    void poll();

    /// @brief L3 TX helper protocols call to emit a built payload on a SPECIFIC
    ///        device.  The caller passes the resolved @p next_hop MAC.  The
    ///        device does the L2 framing (or skips it for loopback).  (FOLD-A/B.)
    cinux::lib::ErrorOr<void> send_l3(NetDevice& dev, const EthAddr& next_hop, uint16_t ethertype,
                                      const uint8_t* l3, uint32_t len);

    /// @brief Lookup a device's IP config by device reference.  (FOLD-B.)
    const InDevice* config_for(const NetDevice& dev) const;

private:
    struct DevSlot {
        NetDevice* dev;
        InDevice   cfg;
    };
    struct ProtoSlot {
        uint16_t         ethertype;
        ProtocolHandler* h;
    };
    DevSlot   devs_[kMaxDevs]{};
    ProtoSlot protos_[kMaxProtos]{};
};

}  // namespace cinux::net
