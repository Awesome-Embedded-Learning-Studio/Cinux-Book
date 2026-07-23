/**
 * @file kernel/test/test_e1000.cpp
 * @brief QEMU in-kernel integration tests for the e1000 NIC (F5-M6 批a)
 *
 * Runs inside the big kernel test suite.  PCI-enumerates an Intel e1000 NIC
 * (vendor 0x8086); if none is present (the default QEMU config has no e1000)
 * the test SKIPS (no failure).  Under run-kernel-test (-device e1000 ...) it
 * exercises the real bring-up: PCI find + BAR0 map + reset + EEPROM MAC read,
 * asserting the MAC is non-zero and reporting link status.
 *
 * Preconditions: VMM initialised (g_vmm.init in main_test.cpp) for the BAR0 map.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/irq.hpp"  // F5-M6: sti/hlt for e1000 RX poll
#include "kernel/drivers/net/e1000.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::drivers::net::E1000Controller;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::pci::PCI;

// ============================================================
// Test 1: find e1000 + read EEPROM MAC (skip if no NIC present)
// ============================================================

namespace test_e1000 {

void test_detect_and_mac() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_e1000(dev)) {
        cinux::lib::kprintf("[e1000] no NIC present -- skipping detect/MAC test\n");
        return;  // counts as a pass when no e1000 is attached
    }

    E1000Controller           nic;
    cinux::lib::ErrorOr<void> r = nic.init(dev);
    TEST_ASSERT_TRUE(r.ok());

    uint8_t mac[6] = {};
    nic.mac(mac);
    cinux::lib::kprintf("[e1000] EEPROM MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%d\n", mac[0],
                        mac[1], mac[2], mac[3], mac[4], mac[5], static_cast<int>(nic.link_up()));

    // A real NIC has a non-zero MAC (QEMU assigns 52:54:00:...).  Reject the
    // all-zero "no EEPROM" read so a silent EERD failure fails loudly.
    bool mac_nonzero = false;
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0) {
            mac_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(mac_nonzero);

    E1000Controller::set_instance(&nic);
}

// ============================================================
// Test 2: TX an ARP request -> SLIRP replies -> RX catches it
// Definitive RX proof via real inbound traffic.  (QEMU's e1000 does NOT honour
// RCTL.LBM_MAC internal loopback -- the bit writes, but a TX'd frame never
// appears in RX -- so we provoke real traffic: ARP "who has 10.0.2.2" makes the
// user-net gateway answer, and our receiver must catch the reply.)
// ============================================================

void test_arp_roundtrip() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_e1000(dev)) {
        cinux::lib::kprintf("[e1000] no NIC present -- skipping ARP test\n");
        return;
    }

    E1000Controller nic;
    if (!nic.init(dev).ok() || !nic.start_rx().ok() || !nic.start_tx().ok()) {
        TEST_ASSERT_TRUE(false);
        return;
    }

    uint8_t mac[6];
    nic.mac(mac);

    // ARP request: "who has 10.0.2.2?" (the QEMU user-net gateway).  SLIRP owns
    // 10.0.2.2 and answers, so the reply is genuine inbound traffic.
    uint8_t arp[60] = {};
    for (int i = 0; i < 6; ++i) {
        arp[i]      = 0xFF;    // dst = broadcast
        arp[6 + i]  = mac[i];  // src / sender HW addr = our MAC
        arp[22 + i] = mac[i];
    }
    arp[12] = 0x08;
    arp[13] = 0x06;  // ethertype = ARP
    arp[14] = 0x00;
    arp[15] = 0x01;  // htype = Ethernet
    arp[16] = 0x08;
    arp[17] = 0x00;  // ptype = IPv4
    arp[18] = 6;     // hlen
    arp[19] = 4;     // plen
    arp[20] = 0x00;
    arp[21] = 0x01;  // op = request
    arp[28] = 10;
    arp[29] = 0;
    arp[30] = 2;
    arp[31] = 15;  // sender IP 10.0.2.15
    arp[38] = 10;
    arp[39] = 0;
    arp[40] = 2;
    arp[41] = 2;  // target IP 10.0.2.2

    TEST_ASSERT_TRUE(nic.send_packet(arp, sizeof(arp)).ok());

    // Poll for SLIRP's ARP reply.  Self-contained on the QEMU user-net gateway:
    // no reply is a real failure (TX up, gateway must answer), not a soft-skip.
    uint8_t  rx[256] = {};
    uint32_t len     = 0;
    bool     got     = false;
    for (uint32_t i = 0; i < 2000000; ++i) {
        if (nic.poll_rx(rx, sizeof(rx), len)) {
            got = true;
            break;
        }
        // No packet yet: sti+hlt lets QEMU's main loop run and pull SLIRP's
        // reply into the ring; the LAPIC timer IRQ (armed in main_test) wakes
        // us to retry.  IF stays off outside this block.
        cinux::arch::irq_enable();
        cinux::arch::hlt();
        cinux::arch::irq_disable();
    }

    if (!got) {
        nic.rx_dump("arp-timeout");
    }
    TEST_ASSERT_TRUE(got);
    cinux::lib::kprintf("[e1000] ARP round-trip: reply len=%u ethertype=%02x%02x op=%02x%02x\n",
                        len, rx[12], rx[13], rx[20], rx[21]);
    // It must be an ARP reply: ethertype 0x0806, op 0x0002.
    TEST_ASSERT_TRUE(rx[12] == 0x08 && rx[13] == 0x06);
    TEST_ASSERT_TRUE(rx[20] == 0x00 && rx[21] == 0x02);
}

// ============================================================
// Test 3: TX a DHCPDISCOVER -> SLIRP answers with a BROADCAST
// DHCPOFFER -> RX catches it (broadcast acceptance / BAM).
// Complements the unicast ARP round-trip.  DHCP is the one reliable
// broadcast source on QEMU user-net: the built-in DHCP server answers
// every discover, and the BOOTP BROADCAST flag makes the offer broadcast.
// ============================================================

void test_broadcast_rx() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_e1000(dev)) {
        cinux::lib::kprintf("[e1000] no NIC present -- skipping broadcast test\n");
        return;
    }

    E1000Controller nic;
    if (!nic.init(dev).ok() || !nic.start_rx().ok() || !nic.start_tx().ok()) {
        TEST_ASSERT_TRUE(false);
        return;
    }

    uint8_t mac[6];
    nic.mac(mac);

    // DHCPDISCOVER: eth(broadcast) / IPv4 / UDP(68->67) / BOOTP(op=1, broadcast
    // flag, chaddr=MAC) / DHCP magic + option 53=Discover + end.  342 bytes.
    uint8_t p[342] = {};
    for (int i = 0; i < 6; ++i) {
        p[i]      = 0xFF;    // dst = broadcast
        p[6 + i]  = mac[i];  // src = our MAC
        p[70 + i] = mac[i];  // BOOTP chaddr (offset 28 within BOOTP@42)
    }
    p[12]  = 0x08;
    p[13]  = 0x00;  // ethertype IPv4
    p[14]  = 0x45;  // IP ver4 / IHL5
    p[16]  = 0x01;
    p[17]  = 0x48;  // IP total length 328
    p[22]  = 0x40;  // TTL 64
    p[23]  = 0x11;  // protocol UDP
    p[30]  = 0xFF;
    p[31]  = 0xFF;
    p[32]  = 0xFF;
    p[33]  = 0xFF;  // IP dst 255.255.255.255
    p[34]  = 0x00;
    p[35]  = 0x44;  // UDP src 68
    p[36]  = 0x00;
    p[37]  = 0x43;  // UDP dst 67
    p[38]  = 0x01;
    p[39]  = 0x34;  // UDP length 308
    p[42]  = 0x01;  // BOOTP op = BOOTREQUEST
    p[43]  = 0x01;  // htype Ethernet
    p[44]  = 0x06;  // hlen
    p[46]  = 0x12;
    p[47]  = 0x34;
    p[48]  = 0x56;
    p[49]  = 0x78;  // xid
    p[52]  = 0x80;
    p[53]  = 0x00;  // flags = BROADCAST
    p[278] = 0x63;
    p[279] = 0x82;
    p[280] = 0x53;
    p[281] = 0x63;  // DHCP magic cookie
    p[282] = 53;
    p[283] = 1;
    p[284] = 1;     // option 53 = DHCP Discover
    p[285] = 0xFF;  // option 255 = end

    // IP header checksum over the 20-byte header (offset 14..33, checksum zeroed).
    uint32_t sum = 0;
    for (int i = 14; i < 34; i += 2) {
        sum += (static_cast<uint32_t>(p[i]) << 8) | p[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    const uint16_t cksum = static_cast<uint16_t>(~sum & 0xFFFF);
    p[24]                = static_cast<uint8_t>(cksum >> 8);
    p[25]                = static_cast<uint8_t>(cksum & 0xFF);

    TEST_ASSERT_TRUE(nic.send_packet(p, sizeof(p)).ok());

    uint8_t  rx[256] = {};
    uint32_t len     = 0;
    bool     got     = false;
    for (uint32_t i = 0; i < 2000000; ++i) {
        if (nic.poll_rx(rx, sizeof(rx), len)) {
            got = true;
            break;
        }
        // No packet yet: sti+hlt lets QEMU's main loop run and pull SLIRP's
        // reply into the ring; the LAPIC timer IRQ (armed in main_test) wakes
        // us to retry.  IF stays off outside this block.
        cinux::arch::irq_enable();
        cinux::arch::hlt();
        cinux::arch::irq_disable();
    }

    if (!got) {
        nic.rx_dump("dhcp-timeout");
    }
    TEST_ASSERT_TRUE(got);
    const bool bcast = rx[0] == 0xFF && rx[1] == 0xFF && rx[2] == 0xFF && rx[3] == 0xFF &&
                       rx[4] == 0xFF && rx[5] == 0xFF;
    cinux::lib::kprintf(
        "[e1000] DHCP offer: len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x ethertype=%02x%02x "
        "bootop=%02x\n",
        len, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[12], rx[13], rx[42]);
    TEST_ASSERT_TRUE(bcast);                             // SLIRP broadcast the offer -> BAM
    TEST_ASSERT_TRUE(rx[12] == 0x08 && rx[13] == 0x00);  // IPv4
    TEST_ASSERT_TRUE(rx[42] == 0x02);                    // BOOTP reply (BOOTREPLY)
}

}  // namespace test_e1000

extern "C" void run_e1000_tests() {
    TEST_SECTION("e1000");
    RUN_TEST(test_e1000::test_detect_and_mac);
    RUN_TEST(test_e1000::test_arp_roundtrip);
    RUN_TEST(test_e1000::test_broadcast_rx);
    TEST_SUMMARY();
}
