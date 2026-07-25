/**
 * @file kernel/test/test_block_device.cpp
 * @brief QEMU in-kernel tests for IBlockDevice + RAMBlockDevice (M4-1)
 *
 * Exercises the block device interface through its in-memory implementation:
 * geometry, create-time validation, write/read round-trips (single and
 * multi-block), out-of-range rejection, a non-default block size, and virtual
 * dispatch through the IBlockDevice base pointer.  No hardware is involved --
 * AHCIBlockDevice is batch 2.  Requires the heap initialised.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ram_block_device.hpp"
#include "kernel/lib/string.hpp"

using cinux::drivers::IBlockDevice;
using cinux::drivers::RAMBlockDevice;
using cinux::lib::Error;

// ============================================================
// Test 1: create succeeds and reports geometry
// ============================================================

namespace test_blk_create {

void test_create_and_geometry() {
    auto r = RAMBlockDevice::create(16);
    TEST_ASSERT_TRUE(r.ok());
    auto& dev = r.value();
    TEST_ASSERT_TRUE(dev.block_count() == 16);
    TEST_ASSERT_TRUE(dev.block_size() == RAMBlockDevice::kDefaultBlockSize);
}

}  // namespace test_blk_create

// ============================================================
// Test 2: zero dimensions are rejected
// ============================================================

namespace test_blk_create_zero {

void test_create_rejects_zero() {
    auto r0 = RAMBlockDevice::create(0);
    TEST_ASSERT_FALSE(r0.ok());
    TEST_ASSERT_TRUE(r0.error() == Error::InvalidArgument);

    auto rz = RAMBlockDevice::create(16, 0);
    TEST_ASSERT_FALSE(rz.ok());
    TEST_ASSERT_TRUE(rz.error() == Error::InvalidArgument);
}

}  // namespace test_blk_create_zero

// ============================================================
// Test 3: single-block write then read round-trip
// ============================================================

namespace test_blk_roundtrip {

void test_write_then_read() {
    auto  r   = RAMBlockDevice::create(16);
    auto& dev = r.value();

    uint8_t pattern[512];
    for (size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>(i);
    }
    TEST_ASSERT_TRUE(dev.write_blocks(3, 1, pattern).ok());

    uint8_t out[512] = {};
    TEST_ASSERT_TRUE(dev.read_blocks(3, 1, out).ok());
    TEST_ASSERT_EQ(memcmp(out, pattern, sizeof(out)), 0);
}

}  // namespace test_blk_roundtrip

// ============================================================
// Test 4: multi-block transfer and partial re-read
// ============================================================

namespace test_blk_multi {

void test_multi_block_transfer() {
    auto  r   = RAMBlockDevice::create(16);
    auto& dev = r.value();

    uint8_t payload[4 * 512];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(0xA0 | (i & 0x0F));
    }
    TEST_ASSERT_TRUE(dev.write_blocks(5, 4, payload).ok());

    uint8_t mirror[4 * 512] = {};
    TEST_ASSERT_TRUE(dev.read_blocks(5, 4, mirror).ok());
    TEST_ASSERT_EQ(memcmp(mirror, payload, sizeof(payload)), 0);

    // a single block inside the written range matches its slice
    uint8_t one[512] = {};
    TEST_ASSERT_TRUE(dev.read_blocks(6, 1, one).ok());
    TEST_ASSERT_EQ(memcmp(one, payload + 512, sizeof(one)), 0);
}

}  // namespace test_blk_multi

// ============================================================
// Test 5: out-of-range transfers are rejected; exact edge allowed
// ============================================================

namespace test_blk_range {

void test_out_of_range_rejected() {
    auto    r        = RAMBlockDevice::create(16);
    auto&   dev      = r.value();
    uint8_t buf[512] = {};

    // block 16 is past the end (valid range 0..15)
    TEST_ASSERT_FALSE(dev.read_blocks(16, 1, buf).ok());
    TEST_ASSERT_FALSE(dev.write_blocks(16, 1, buf).ok());
    TEST_ASSERT_TRUE(dev.read_blocks(16, 1, buf).error() == Error::InvalidArgument);

    // count overruns the device: start 14, ask for 4 (needs up to 17)
    TEST_ASSERT_FALSE(dev.read_blocks(14, 4, buf).ok());

    // exact-edge transfer at the last block succeeds
    TEST_ASSERT_TRUE(dev.write_blocks(15, 1, buf).ok());
    TEST_ASSERT_TRUE(dev.read_blocks(15, 1, buf).ok());
}

}  // namespace test_blk_range

// ============================================================
// Test 6: non-default block size
// ============================================================

namespace test_blk_custom_size {

void test_custom_block_size() {
    auto  r   = RAMBlockDevice::create(8, 1024);
    auto& dev = r.value();
    TEST_ASSERT_TRUE(dev.block_count() == 8);
    TEST_ASSERT_TRUE(dev.block_size() == 1024);

    uint8_t buf[1024];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = static_cast<uint8_t>(0x5A ^ (i & 0xFF));
    }
    TEST_ASSERT_TRUE(dev.write_blocks(2, 1, buf).ok());
    uint8_t out[1024] = {};
    TEST_ASSERT_TRUE(dev.read_blocks(2, 1, out).ok());
    TEST_ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);
}

}  // namespace test_blk_custom_size

// ============================================================
// Test 7: virtual dispatch through the IBlockDevice base pointer
// ============================================================

namespace test_blk_polymorphic {

void test_polymorphic_via_base() {
    auto          r    = RAMBlockDevice::create(16);
    auto&         dev  = r.value();
    IBlockDevice* base = &dev;  // exercise the virtual dispatch

    TEST_ASSERT_TRUE(base->block_count() == 16);
    TEST_ASSERT_TRUE(base->block_size() == 512);

    uint8_t buf[512];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = 0x77;
    }
    TEST_ASSERT_TRUE(base->write_blocks(0, 1, buf).ok());
    uint8_t out[512] = {};
    TEST_ASSERT_TRUE(base->read_blocks(0, 1, out).ok());
    TEST_ASSERT_EQ(memcmp(out, buf, sizeof(out)), 0);

    TEST_ASSERT_TRUE(base->flush().ok());  // default no-op succeeds
}

}  // namespace test_blk_polymorphic

// ============================================================
// Entry point
// ============================================================

extern "C" void run_block_device_tests() {
    TEST_SECTION("Block Device Tests (M4-1)");

    RUN_TEST(test_blk_create::test_create_and_geometry);
    RUN_TEST(test_blk_create_zero::test_create_rejects_zero);
    RUN_TEST(test_blk_roundtrip::test_write_then_read);
    RUN_TEST(test_blk_multi::test_multi_block_transfer);
    RUN_TEST(test_blk_range::test_out_of_range_rejected);
    RUN_TEST(test_blk_custom_size::test_custom_block_size);
    RUN_TEST(test_blk_polymorphic::test_polymorphic_via_base);

    TEST_SUMMARY();
}
