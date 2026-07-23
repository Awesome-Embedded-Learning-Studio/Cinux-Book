/**
 * @file kernel/drivers/net/e1000_init.hpp
 * @brief Boot-time e1000 NIC bring-up (F5-M6)
 *
 * Wires the e1000 driver into the boot path: discovers the NIC via PCI, brings
 * it up (BAR0 map + reset + EEPROM MAC + link), arms the legacy RX ring, and
 * publishes it via E1000Controller::set_instance.  Graceful no-op if no e1000 is
 * present, so a boot with no -device e1000 stays clean.
 *
 * §14 file gate: when CINUX_NET is off, e1000_init.cpp is absent and a no-op
 * stub of the same name (net_stub.cpp) is linked instead -- callers (main.cpp)
 * need no #ifdef.
 *
 * Namespace: cinux::drivers::net
 */

#pragma once

namespace cinux::drivers::net {

/// Bring up the e1000 NIC at boot (see file header).  No-op if no NIC present.
void init();

}  // namespace cinux::drivers::net
