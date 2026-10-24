/**
 * @file    ifaces.hpp
 * @brief   The interface roll: what ifconfig and /proc/net/dev read.
 *
 * The stack knows two interfaces at most — the loopback the boot
 * always wires, and the one real wire the boot may have found. This
 * file turns that knowledge into a roll anyone may read: enumerate by
 * ordinal, or look one up by its name. The facts are plain values,
 * copied out under no promise they stay true; the roll is a snapshot
 * service, not a registry to write. Traffic counters are deliberately
 * absent — nothing in the family books per-interface bytes yet, and a
 * zero the kernel does not honestly track would be a made-up number.
 *
 * @author  Charliechen114514
 * @date    2026-10-24
 * @version 0.1
 * @since   0.7.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"

namespace cinux::net {

/**
 * @brief   The loopback interface's name, as the roll spells it.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr char kLoopbackName[] = "lo";

/**
 * @brief   The wired interface's name, as the roll spells it.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr char kWireName[] = "eth0";

/**
 * @brief   The longest name the roll spells, IFNAMSIZ-shaped.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kIfNameMax = 16;

/**
 * @brief   The hardware-type words the roll answers, Linux-shaped.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kHardwareLoopback = 772;
inline constexpr uint16_t kHardwareEthernet = 1;

/**
 * @brief   One interface's plain facts, as the roll hands them out.
 * @since   0.7.0
 * @ingroup kernel_net
 */
struct InterfaceFacts {
    /// The interface's address, host order.
    uint32_t                address       = 0;
    /// The network's mask, host order.
    uint32_t                netmask       = 0;
    /// The network's broadcast, host order.
    uint32_t                broadcast     = 0;
    /// The hardware-type word: loopback or Ethernet.
    uint16_t                hardware_type = 0;
    /// The largest frame the interface carries; the loopback's own
    /// answer crosses the sixteen-bit line, and ifr_mtu is an int.
    uint32_t                mtu           = 0;
    /// The hardware address; all zero for the loopback.
    driver::net::MacAddress hardware      = {};
};

/**
 * @brief         Reads one interface's facts by name.
 *
 * @param[in]     name   The interface's name, NUL-terminated.
 * @param[out]    into   The facts, on success.
 * @return        True when the roll holds that name; false for a
 *                stranger or before the world wired itself.
 * @since         0.7.0
 * @ingroup       kernel_net
 */
[[nodiscard]] bool ReadInterfaceFacts(const char* name, InterfaceFacts& into);

/**
 * @brief         Names the ordinal-th interface of the roll.
 *
 * @param[in]     ordinal   The seat to read, zero first.
 * @param[out]    out       Where the NUL-terminated name lands; the
 *                          buffer must hold the longest name plus one.
 * @return        True when that ordinal holds an interface; false
 *                past the end of the roll.
 * @note          The loopback comes first, the wire second when one
 *                        joined. Enumeration order is the roll's own
 *                        constant, not a promise about seating races.
 * @since         0.7.0
 * @ingroup       kernel_net
 */
[[nodiscard]] bool NameInterfaceAt(unsigned long ordinal, char* out);

/**
 * @brief         How many interfaces the roll holds.
 * @return        One or two: the loopback always, the wire when
 *                attached.
 * @since         0.7.0
 * @ingroup       kernel_net
 */
[[nodiscard]] unsigned long InterfaceCount();

}  // namespace cinux::net
