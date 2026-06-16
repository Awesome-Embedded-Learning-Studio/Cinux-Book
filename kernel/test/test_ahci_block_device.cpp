/**
 * @file kernel/test/test_ahci_block_device.cpp
 * @brief QEMU in-kernel tests for AHCIBlockDevice (M4-2)
 *
 * Drives the IBlockDevice adapter over real QEMU AHCI hardware: create()
 * allocates its DMA buffer, read_blocks fetches sector 0 and returns the MBR
 * boot signature, write_blocks round-trips a block, and the base IBlockDevice
 * pointer exercises virtual dispatch.  Each test brings up its own PCI/AHCI
 * instance (as test_ahci.cpp does); the adapter is created on top.
 *
 * Preconditions: GDT/IDT, PMM, VMM, heap initialised (main_test runs these
 * before the AHCI suites).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/drivers/ahci/ahci_config.hpp"
#include "kernel/drivers/block_device.hpp"
#include "kernel/drivers/pci/pci.hpp"

using cinux::drivers::ahci::AHCIBlockDevice;
using cinux::drivers::ahci::AHCI;
using cinux::drivers::ahci::SECTOR_SIZE;
using cinux::drivers::IBlockDevice;
using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;

// ============================================================
// Test 1: create succeeds and reports 512-byte blocks
// ============================================================

namespace test_ahciblk_create {

void test_create_and_block_size() {
    PCI pci;
    pci.init();
    PCIDevice dev{};
    TEST_ASSERT_TRUE(pci.find_ahci(dev));

    AHCI ahci;
    ahci.init(dev);
    TEST_ASSERT_NOT_NULL(ahci.hba_mem());

    auto r = AHCIBlockDevice::create(ahci, 0);
    TEST_ASSERT_TRUE(r.ok());
    auto& blk = r.value();
    TEST_ASSERT_TRUE(blk.block_size() == SECTOR_SIZE);
    TEST_ASSERT_TRUE(blk.block_size() == 512);
}

}  // namespace test_ahciblk_create

// ============================================================
// Test 2: read_blocks fetches sector 0 with the MBR signature
// ============================================================

namespace test_ahciblk_read {

void test_read_sector0_signature() {
    PCI pci;
    pci.init();
    PCIDevice dev{};
    TEST_ASSERT_TRUE(pci.find_ahci(dev));
    AHCI ahci;
    ahci.init(dev);

    auto  r   = AHCIBlockDevice::create(ahci, 0);
    auto& blk = r.value();

    uint8_t buf[512] = {};
    TEST_ASSERT_TRUE(blk.read_blocks(0, 1, buf).ok());
    TEST_ASSERT_EQ(buf[510], 0x55u);
    TEST_ASSERT_EQ(buf[511], 0xAAu);
}

}  // namespace test_ahciblk_read

// ============================================================
// Test 3: write then read round-trips on a safe high block
// ============================================================

namespace test_ahciblk_roundtrip {

void test_write_read_roundtrip() {
    PCI pci;
    pci.init();
    PCIDevice dev{};
    TEST_ASSERT_TRUE(pci.find_ahci(dev));
    AHCI ahci;
    ahci.init(dev);

    auto  r   = AHCIBlockDevice::create(ahci, 0);
    auto& blk = r.value();

    // Sector 2000 (ext2 block 1000) is the same block test_ahci_write uses and
    // is known writable.  Save and restore it so the test leaves the disk
    // untouched for the ext2 suites that run afterwards.
    uint8_t original[512];
    TEST_ASSERT_TRUE(blk.read_blocks(2000, 1, original).ok());

    uint8_t pattern[512];
    for (size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>(0x5A ^ (i & 0xFF));
    }
    TEST_ASSERT_TRUE(blk.write_blocks(2000, 1, pattern).ok());

    uint8_t out[512] = {};
    TEST_ASSERT_TRUE(blk.read_blocks(2000, 1, out).ok());
    TEST_ASSERT_EQ(memcmp(out, pattern, sizeof(out)), 0);

    // Restore the original contents so later ext2 suites see a clean disk.
    TEST_ASSERT_TRUE(blk.write_blocks(2000, 1, original).ok());
}

}  // namespace test_ahciblk_roundtrip

// ============================================================
// Test 4: virtual dispatch through the IBlockDevice base pointer
// ============================================================

namespace test_ahciblk_polymorphic {

void test_polymorphic_via_base() {
    PCI pci;
    pci.init();
    PCIDevice dev{};
    TEST_ASSERT_TRUE(pci.find_ahci(dev));
    AHCI ahci;
    ahci.init(dev);

    auto          r    = AHCIBlockDevice::create(ahci, 0);
    auto&         blk  = r.value();
    IBlockDevice* base = &blk;

    TEST_ASSERT_TRUE(base->block_size() == 512);
    uint8_t buf[512] = {};
    TEST_ASSERT_TRUE(base->read_blocks(0, 1, buf).ok());
    TEST_ASSERT_EQ(buf[510], 0x55u);
    TEST_ASSERT_EQ(buf[511], 0xAAu);
    TEST_ASSERT_TRUE(base->flush().ok());  // inherited no-op succeeds
}

}  // namespace test_ahciblk_polymorphic

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ahci_block_device_tests() {
    TEST_SECTION("AHCI Block Device Tests (M4-2)");

    RUN_TEST(test_ahciblk_create::test_create_and_block_size);
    RUN_TEST(test_ahciblk_read::test_read_sector0_signature);
    RUN_TEST(test_ahciblk_roundtrip::test_write_read_roundtrip);
    RUN_TEST(test_ahciblk_polymorphic::test_polymorphic_via_base);

    TEST_SUMMARY();
}
