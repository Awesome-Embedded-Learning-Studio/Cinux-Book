/**
 * @file kernel/test/test_ext4_extents.cpp
 * @brief QEMU in-kernel test for ext4 extent-tree reads (F6-M5)
 *
 * Mounts the dedicated ext4 (extents) disk image on AHCI port 2 through the
 * ext2 driver and verifies that extent-mapped inodes (EXT4_EXTENTS_FL) read
 * back correctly:
 *   - The volume advertises the ext4 extents incompat feature.
 *   - A 1 MiB file (single depth-0 leaf extent, 1024 blocks @ 1 KB) reads back
 *     its full byte[i] == i & 0xFF pattern end-to-end.
 *   - Reads that cross a block boundary resolve correctly.
 *   - A small single-block-extent file reads back its text.
 *
 * The ext4 image is built by scripts/create_ext4_disk.sh and attached at
 * ahci.2 (port 0 = AHCI raw test disk, port 1 = ext2 disk).
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM/VMM/Heap initialised (DMA buffers + new/delete)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "libs/ext2/ext2.hpp"
#include "kernel/lib/string.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::fs::Ext2;
using cinux::fs::Ext2CachedInode;
using cinux::fs::EXT4_EXTENTS_FL;
using cinux::fs::Inode;
using cinux::fs::InodeType;

// Size of /big.bin on the ext4 image (1 MiB), set by create_ext4_disk.sh.
static constexpr uint64_t BIG_FILE_SIZE = 1048576;

namespace {

struct AhciExt4Pair {
    AHCI*                                  ahci;
    Ext2*                                  ext2;
    cinux::drivers::ahci::AHCIBlockDevice* blk_dev;
};

/// PCI -> AHCI -> AHCIBlockDevice on port 2 (the ext4 extents disk) -> Ext2 mount.
AhciExt4Pair setup_ext4() {
    AhciExt4Pair result{nullptr, nullptr, nullptr};

    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);
    if (result.ahci->hba_mem() == nullptr) {
        return result;
    }

    auto blk = cinux::drivers::ahci::AHCIBlockDevice::create(*result.ahci, 2);
    result.blk_dev =
        blk.ok() ? new cinux::drivers::ahci::AHCIBlockDevice(std::move(blk.value())) : nullptr;
    result.ext2 = new Ext2(result.blk_dev);
    ASSERT_OK(result.ext2->mount());

    return result;
}

void teardown_ext4(AhciExt4Pair& pair) {
    delete pair.ext2;
    delete pair.blk_dev;
    delete pair.ahci;
    pair.ext2 = nullptr;
    pair.ahci = nullptr;
}

}  // anonymous namespace

// ============================================================
// Test 1: mount detects the ext4 extents volume
// ============================================================

namespace test_ext4_mount {

void test_mount_ext4_volume() {
    auto pair = setup_ext4();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // The ext4 image sets EXT4_FEATURE_INCOMPAT_EXTENTS in the superblock.
    TEST_ASSERT_TRUE(pair.ext2->has_ext4_extents_feature());

    teardown_ext4(pair);
}

}  // namespace test_ext4_mount

// ============================================================
// Test 2: /big.bin is extent-mapped and reads back byte-exact
// ============================================================

namespace test_ext4_big_file {

void test_big_file_uses_extents() {
    auto pair = setup_ext4();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    Inode* ino = lookup_or_null(pair.ext2, "big.bin");
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_EQ(static_cast<uint32_t>(ino->type), static_cast<uint32_t>(InodeType::Regular));

    // The inode must actually be extent-mapped -- otherwise the read below would
    // fall through to the (wrong) indirect-block path.
    auto* cached = static_cast<const Ext2CachedInode*>(ino->fs_private);
    TEST_ASSERT_TRUE((cached->disk_inode.i_flags & EXT4_EXTENTS_FL) != 0);

    teardown_ext4(pair);
}

void test_big_file_full_pattern() {
    auto pair = setup_ext4();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    Inode* ino = lookup_or_null(pair.ext2, "big.bin");
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_EQ(ino->size, BIG_FILE_SIZE);

    // Read the whole file in 8 KiB chunks and verify byte[i] == i & 0xFF.
    // Exercises multi-block reads within one extent plus the tail block.
    static uint8_t buf[8192];
    uint64_t       pos = 0;
    bool           ok  = true;

    while (pos < BIG_FILE_SIZE) {
        uint64_t want = BIG_FILE_SIZE - pos;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }

        int64_t n = read_or_neg1(ino, pos, buf, want);
        if (n != static_cast<int64_t>(want)) {
            ok = false;
            break;
        }
        for (uint64_t k = 0; k < want; ++k) {
            if (buf[k] != static_cast<uint8_t>((pos + k) & 0xFF)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        pos += static_cast<uint64_t>(n);
    }

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(pos, BIG_FILE_SIZE);

    teardown_ext4(pair);
}

void test_big_file_cross_block_read() {
    auto pair = setup_ext4();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    Inode* ino = lookup_or_null(pair.ext2, "big.bin");
    TEST_ASSERT_NOT_NULL(ino);

    // Read 8 bytes straddling the first 1 KB block boundary (offset 1020..1027).
    // Verifies the extent resolver + block-offset math at a boundary.
    uint8_t buf[8] = {};
    int64_t n      = read_or_neg1(ino, 1020, buf, sizeof(buf));
    TEST_ASSERT_EQ(n, static_cast<int64_t>(sizeof(buf)));
    for (uint64_t k = 0; k < sizeof(buf); ++k) {
        TEST_ASSERT_EQ(buf[k], static_cast<uint8_t>((1020 + k) & 0xFF));
    }

    teardown_ext4(pair);
}

}  // namespace test_ext4_big_file

// ============================================================
// Test 3: /small.txt single-block extent reads back its text
// ============================================================

namespace test_ext4_small_file {

void test_small_file_content() {
    auto pair = setup_ext4();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    Inode* ino = lookup_or_null(pair.ext2, "small.txt");
    TEST_ASSERT_NOT_NULL(ino);

    char    buf[64] = {};
    int64_t n       = read_or_neg1(ino, 0, buf, sizeof(buf) - 1);
    // "ext4 extents small file\n" is exactly 24 bytes (printf in the image script).
    TEST_ASSERT_EQ(n, 24);

    // Mirrors the content written by create_ext4_disk.sh (trailing newline).
    TEST_ASSERT_TRUE(strcmp(buf, "ext4 extents small file\n") == 0);

    teardown_ext4(pair);
}

}  // namespace test_ext4_small_file

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ext4_extents_tests() {
    TEST_SECTION("Ext4 Extents Tests (F6-M5)");

    RUN_TEST(test_ext4_mount::test_mount_ext4_volume);

    RUN_TEST(test_ext4_big_file::test_big_file_uses_extents);
    RUN_TEST(test_ext4_big_file::test_big_file_full_pattern);
    RUN_TEST(test_ext4_big_file::test_big_file_cross_block_read);

    RUN_TEST(test_ext4_small_file::test_small_file_content);

    TEST_SUMMARY();
}
