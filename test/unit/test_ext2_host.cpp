/**
 * @file test/unit/test_ext2_host.cpp
 * @brief F6-M6 B1b: ext2 real-logic host test (ASAN)
 *
 * Mounts the prebuilt 64 KiB ext2 image (test/data/ext2_test.img, populated by
 * mke2fs -d with /etc/motd + /hello.txt) into a RAMBlockDevice and exercises the
 * full VFS path through the REAL ext2 driver: readdir, multi-level lookup, file
 * read, and the mutators create/write/read-back/unlink. Runs under ASAN so any
 * UAF / OOB / leak in the inode cache, block allocator, or scratch buffers
 * surfaces in milliseconds (the host counterpart of the F-DYN-COV QEMU
 * forensics). The image path is injected by CMake (EXT2_TEST_IMG_PATH).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>
#    include <fstream>
#    include <iterator>
#    include <vector>

#    include <cinux/expected.hpp>
#    include "kernel/drivers/ram_block_device.hpp"
#    include "kernel/fs/file.hpp"  // inode_unref (cache returns ref'd inodes)
#    include "libs/ext2/ext2.hpp"

#    ifndef EXT2_TEST_IMG_PATH
#        error "EXT2_TEST_IMG_PATH must be defined by CMake (source path to the image)"
#    endif

using cinux::drivers::RAMBlockDevice;
using cinux::fs::Ext2;
using cinux::fs::Inode;
using cinux::fs::inode_unref;

// Drive the real ext2 driver over a RAMBlockDevice loaded from the prebuilt
// image: mount, readdir, read, create/write/read-back, unlink. Every returned
// inode is ref'd by get_cached_inode(), so each is paired with inode_unref
// before ~Ext2 runs.
TEST("ext2_host: mount + readdir + read + create/write/readback + unlink") {
    // --- load the prebuilt image into a RAMBlockDevice ---
    std::ifstream img(EXT2_TEST_IMG_PATH, std::ios::binary);
    ASSERT_TRUE(img.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(img)),
                            std::istreambuf_iterator<char>());
    ASSERT_TRUE(bytes.size() > 0 && bytes.size() % 512 == 0);
    const uint64_t nblocks = bytes.size() / 512;
    auto dev_r = RAMBlockDevice::create(nblocks);
    ASSERT_OK(dev_r);
    RAMBlockDevice dev = std::move(*dev_r);
    ASSERT_OK(dev.write_blocks(0, nblocks, bytes.data()));

    Ext2 ext2(&dev);
    ASSERT_OK(ext2.mount());

    // --- readdir "/" : expect etc/ and hello.txt (index 0/1 are "." / "..") ---
    auto root_r = ext2.lookup("/");
    ASSERT_OK(root_r);
    Inode* root = *root_r;
    bool found_etc = false, found_hello = false;
    for (uint64_t i = 2; i < 64; ++i) {
        char nm[256];
        auto r = root->ops->readdir(root, i, nm, 255);
        ASSERT_TRUE(r.ok());
        if (*r == 0) break;  // end of directory
        if (std::strcmp(nm, "etc") == 0) found_etc = true;
        if (std::strcmp(nm, "hello.txt") == 0) found_hello = true;
    }
    ASSERT_TRUE(found_etc);
    ASSERT_TRUE(found_hello);

    // --- multi-level lookup + read /etc/motd ---
    auto motd_r = ext2.lookup("/etc/motd");
    ASSERT_OK(motd_r);
    Inode* motd = *motd_r;
    {
        char buf[64];
        auto rd = motd->ops->read(motd, 0, buf, sizeof(buf) - 1);
        ASSERT_TRUE(rd.ok());
        ASSERT_GT(*rd, 0);
        buf[*rd] = '\0';
        ASSERT_TRUE(std::strstr(buf, "hello ext2 motd") != nullptr);
    }

    // --- create /newfile + write + read-back (exercises block allocator) ---
    auto newf_r = root->ops->create(root, "newfile", 7);
    ASSERT_OK(newf_r);
    Inode* newf = *newf_r;
    {
        const char  payload[] = "hello-write";
        const auto  plen = static_cast<int64_t>(sizeof(payload) - 1);
        auto        wr = newf->ops->write(newf, 0, payload, static_cast<uint64_t>(plen));
        ASSERT_TRUE(wr.ok());
        ASSERT_EQ(*wr, plen);

        char  rb[32];
        auto  rb_r = newf->ops->read(newf, 0, rb, sizeof(rb) - 1);
        ASSERT_TRUE(rb_r.ok());
        ASSERT_EQ(*rb_r, plen);
        rb[*rb_r] = '\0';
        ASSERT_TRUE(std::strstr(rb, "hello-write") != nullptr);
    }

    // --- unlink /newfile, then lookup must fail ---
    ASSERT_OK(root->ops->unlink(root, "newfile", 7));
    {
        auto gone = ext2.lookup("/newfile");
        ASSERT_FALSE(gone.ok());
    }

    // --- drop refs before ~Ext2 tears down the inode cache ---
    inode_unref(root);
    inode_unref(motd);
    inode_unref(newf);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
