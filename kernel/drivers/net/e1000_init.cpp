/**
 * @file kernel/drivers/net/e1000_init.cpp
 * @brief Boot-time e1000 bring-up: PCI find + init + arm RX
 *
 * Called from kernel main after the AHCI block.  Finds the first e1000 NIC on
 * the PCI bus, brings it up + arms RX, and publishes the singleton.  No consumer
 * drains received packets yet (the F7 protocol stack will); the ring buffers
 * frames until then.  Graceful no-op if no NIC is present.
 *
 * Namespace: cinux::drivers::net
 */

#include "e1000_init.hpp"

#include "e1000.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers::net {

void init() {
    pci::PCI       pci;
    pci::PCIDevice dev{};
    if (!pci.find_e1000(dev)) {
        cinux::lib::kprintf("[e1000] No NIC found -- net disabled\n");
        return;
    }

    static E1000Controller nic;  // lives for the kernel's lifetime
    if (!nic.init(dev).ok()) {
        cinux::lib::kprintf("[e1000] init failed -- net disabled\n");
        return;
    }
    if (!nic.start_rx().ok()) {
        cinux::lib::kprintf("[e1000] RX arm failed -- net disabled\n");
        return;
    }
    // F7 L2: arm TX too.  The L3 stack sends ARP replies + ICMP echo replies, so
    // production init must arm the TX ring (the bring-up tests armed TX themselves;
    // production init previously armed RX only).  Without this, send_l3 composes
    // a frame but it never reaches the wire (silent -- SLIRP never answers).
    if (!nic.start_tx().ok()) {
        cinux::lib::kprintf("[e1000] TX arm failed -- net disabled\n");
        return;
    }
    E1000Controller::set_instance(&nic);
    cinux::lib::kprintf("[e1000] NIC up (singleton published)\n");
}

}  // namespace cinux::drivers::net
