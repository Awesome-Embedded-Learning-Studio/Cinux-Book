/**
 * @file test/unit/test_ext2_host_link.cpp
 * @brief B1a smoke: prove libs/ext2/ compiles + links on host (M6 gate)
 *
 * Not a semantics test -- it only constructs an Ext2 over a RAMBlockDevice and
 * calls mount() (expected to fail with IOError on a zeroed device). If this
 * links and returns without crashing, the host PAL coverage is complete:
 *   cinux::lib::kprintf / kvprintf / kpanic   (ext2_host_pal.cpp)
 *   cinux::mm::kmalloc / kfree                (ext2_host_pal.cpp, RAMBlockDevice)
 *   cinux::proc::Spinlock                     (host_spinlock.cpp)
 *   inode_ref / inode_unref                   (kernel/fs/file.cpp)
 *   InodeOps default methods                  (kernel/fs/inode.cpp)
 * Real fs semantics (mount a real image, lookup, read, write) live in
 * test_ext2_host.cpp (B1b). This is the M6 make-or-break gate.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <utility>

#    include <cinux/expected.hpp>
#    include "kernel/drivers/ram_block_device.hpp"
#    include "libs/ext2/ext2.hpp"

using cinux::drivers::RAMBlockDevice;
using cinux::fs::Ext2;

// B1a: the whole point is the link. We exercise Ext2's ctor + mount + dtor over
// a RAMBlockDevice so every PAL symbol the driver touches gets pulled in. A
// zeroed device fails mount at the superblock magic check (magic 0 != 0xEF53) --
// that is fine; returning without crashing is the gate.
TEST("ext2_host_link: Ext2 ctor + mount + dtor over RAMBlockDevice") {
    auto dev_result = RAMBlockDevice::create(128);  // 64 KB of 512-byte blocks
    ASSERT_OK(dev_result);
    RAMBlockDevice dev = std::move(*dev_result);
    Ext2           ext2(&dev);

    auto mount_result = ext2.mount();
    // Zeroed image -> superblock magic mismatch -> not ok. We only need the call
    // to return (not crash); the link itself is what this test gates.
    ASSERT_FALSE(mount_result.ok());
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
