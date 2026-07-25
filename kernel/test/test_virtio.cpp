/**
 * @file kernel/test/test_virtio.cpp
 * @brief QEMU in-kernel integration tests for VirtIO (F5-M2 batch 1+2+4)
 *
 * Batch 1: transport bring-up.  Batch 2: virtio-blk read/write round-trip.
 * Batch 4: virtio-net bring-up (MAC + RX/TX queue config).  SLIRP traffic is
 * a production/follow-up gate (the test attaches no netdev to virtio-net-pci).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/virtio/virtio.hpp"
#include "kernel/drivers/virtio/virtio_blk.hpp"
#include "kernel/drivers/virtio/virtio_net.hpp"
#include "kernel/drivers/virtio/virtqueue.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/net/net_types.hpp"

using cinux::drivers::pci::PCIDevice;
using cinux::drivers::pci::PCI;
using namespace cinux::drivers;
using namespace cinux::drivers::virtio;

namespace test_virtio {

void test_transport_bringup() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_virtio_block(dev)) {
        cinux::lib::kprintf("[VirtIO] no device present -- skipping transport test\n");
        return;
    }
    TEST_ASSERT_EQ(dev.vendor_id, pci::VirtioPci::VENDOR);

    VirtIODevice            vdev;
    cinux::lib::ErrorOr<void> r = vdev.init(dev);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(vdev.present());

    const VirtioCapLocations& c = vdev.caps();
    TEST_ASSERT_TRUE(c.found_common);
    TEST_ASSERT_TRUE(c.found_notify);
    TEST_ASSERT_TRUE(c.found_isr);
    TEST_ASSERT_TRUE(c.found_device);

    cinux::lib::ErrorOr<uint64_t> fr = vdev.negotiate_features(Feature::VERSION_1);
    TEST_ASSERT_TRUE(fr.ok());
    TEST_ASSERT_TRUE((fr.value() & Feature::VERSION_1) != 0);

    VirtQueue                   vq;
    cinux::lib::ErrorOr<void> qr = vq.init(&vdev, 0, 64);
    TEST_ASSERT_TRUE(qr.ok());
    TEST_ASSERT_EQ(vq.size(), 64u);

    vdev.set_status(Status::DRIVER_OK);
    TEST_ASSERT_TRUE((vdev.status() & Status::DRIVER_OK) != 0);

    cinux::lib::kprintf("[VirtIO] transport OK: negotiated=0x%llx status=0x%x\n",
                        static_cast<unsigned long long>(fr.value()), vdev.status());
}

void test_blk_round_trip() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_virtio_block(dev)) {
        cinux::lib::kprintf("[VirtIO-blk] no device -- skipping round-trip\n");
        return;
    }

    VirtIODevice vdev;
    TEST_ASSERT_TRUE(vdev.init(dev).ok());
    TEST_ASSERT_TRUE(vdev.negotiate_features(Feature::VERSION_1).ok());

    const uint64_t capacity = vdev.device_cfg_read64(0);
    TEST_ASSERT_GT(capacity, 0u);

    auto br = VirtIOBlock::create(vdev, capacity);
    TEST_ASSERT_TRUE(br.ok());
    auto blk = std::move(br.value());
    vdev.set_status(Status::DRIVER_OK);

    static uint8_t wpat[512];
    static uint8_t rpat[512];
    for (uint32_t i = 0; i < sizeof(wpat); ++i) {
        wpat[i] = static_cast<uint8_t>(0x5A ^ (i & 0x1F));
    }
    for (uint32_t i = 0; i < sizeof(rpat); ++i) {
        rpat[i] = 0;
    }
    TEST_ASSERT_TRUE(blk.write_blocks(0, 1, wpat).ok());
    TEST_ASSERT_TRUE(blk.read_blocks(0, 1, rpat).ok());

    bool match = true;
    for (uint32_t i = 0; i < sizeof(wpat); ++i) {
        if (rpat[i] != wpat[i]) {
            cinux::lib::kprintf("[VirtIO-blk] mismatch @%u w=0x%x r=0x%x\n", i, wpat[i], rpat[i]);
            match = false;
            break;
        }
    }
    TEST_ASSERT_TRUE(match);
    cinux::lib::kprintf("[VirtIO-blk] round-trip OK (512B write+read, capacity=%llu sectors)\n",
                        static_cast<unsigned long long>(capacity));
}

void test_net_bringup() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_virtio_net(dev)) {
        cinux::lib::kprintf("[VirtIO-net] no device -- skipping bringup\n");
        return;
    }

    VirtIODevice vdev;
    TEST_ASSERT_TRUE(vdev.init(dev).ok());
    TEST_ASSERT_TRUE(vdev.negotiate_features(Feature::VERSION_1).ok());

    auto nr = VirtIONetDevice::create(vdev);
    TEST_ASSERT_TRUE(nr.ok());
    auto net = std::move(nr.value());
    vdev.set_status(Status::DRIVER_OK);

    cinux::net::EthAddr mac{};
    TEST_ASSERT_TRUE(net.mac(mac));
    // A real NIC has a non-zero MAC (QEMU virtio-net-pci assigns one).
    bool mac_nonzero = false;
    for (int i = 0; i < 6; ++i) {
        if (mac.oct[i] != 0) {
            mac_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(mac_nonzero);
    cinux::lib::kprintf("[VirtIO-net] bringup OK MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                        mac.oct[0], mac.oct[1], mac.oct[2], mac.oct[3], mac.oct[4], mac.oct[5]);
}

}  // namespace test_virtio

extern "C" void run_virtio_tests() {
    TEST_SECTION("VirtIO");
    RUN_TEST(test_virtio::test_transport_bringup);
    RUN_TEST(test_virtio::test_blk_round_trip);
    RUN_TEST(test_virtio::test_net_bringup);
    TEST_SUMMARY();
}
