/**
 * @file kernel/test/test_mount.cpp
 * @brief In-kernel mechanism tests for sys_mount / sys_umount2 (F6-M1)
 *
 * Runs inside QEMU via run-kernel-test.  Drives the kernel-internal do_mount_kernel
 * / do_umount2_kernel directly (the sys_ wrappers only add SMAP user-pointer
 * reads, which a ring-0 test cannot route through is_user_vaddr).  Each test
 * mounts at a unique /tmp_t* path and umounts in cleanup -- the global mount
 * table is shared across test sections (like the fd table), so leaving a mount
 * behind would poison later sections.
 *
 * Coverage: tmpfs mount+resolve+create/write/read, proc/devfs singleton sharing
 * (B1a), ext2 over a block device (B1b: source /dev/sda -> IBlockDevice -> new
 * Ext2), umount semantics, and unknown fstype / missing-target errors.
 */

#include <stdint.h>

#include "kernel/drivers/block_registry.hpp"  // BlockRegistry (ext2 factory test)
#include "kernel/errno.hpp"
#include "kernel/fs/devfs/devfs.hpp"    // devfs::instance / devfs::init
#include "kernel/fs/procfs/procfs.hpp"  // procfs::instance / procfs::init
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/string.hpp"  // memcmp / strlen
#include "kernel/syscall/sys_mount.hpp"
#include "kernel/syscall/sys_umount2.hpp"
#include "kernel/test/big_kernel_test.h"

using cinux::fs::FileSystem;
using cinux::fs::vfs_resolve;
using cinux::syscall::do_mount_kernel;
using cinux::syscall::do_umount2_kernel;

namespace {

/// A unique mount path per test so concurrent test sections never collide on a
/// slot, and so a forgotten umount is easy to spot in the boot log.
constexpr const char* kPathA = "/tmp_t_a";
constexpr const char* kPathB = "/tmp_t_b";

uint32_t nlen(const char* s) {
    return static_cast<uint32_t>(strlen(s));
}

/// Resolve @p path to its mounted FileSystem, or nullptr.
FileSystem* fs_at(const char* path) {
    const char* rel = nullptr;
    FileSystem* fs  = vfs_resolve(path, &rel);
    return fs;
}

}  // namespace

namespace test_mount {

void test_mount_tmpfs_resolves() {
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "tmpfs", 0), 0);
    TEST_ASSERT_NOT_NULL(fs_at(kPathA));
    TEST_ASSERT_EQ(do_umount2_kernel(kPathA, 0), 0);
    TEST_ASSERT_NULL(fs_at(kPathA));  // detached
}

void test_mount_then_create_write_read() {
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathB, "tmpfs", 0), 0);
    const char* rel = nullptr;
    auto*       fs  = vfs_resolve(kPathB, &rel);
    TEST_ASSERT_NOT_NULL(fs);

    auto* root = lookup_or_null(fs, "");
    TEST_ASSERT_NOT_NULL(root);
    auto* f = create_or_null(root, "gcc_out.o", nlen("gcc_out.o"));
    TEST_ASSERT_NOT_NULL(f);

    const char payload[] = "ELF tmp";
    TEST_ASSERT_EQ(write_or_neg1(f, 0, payload, sizeof(payload) - 1),
                   static_cast<int64_t>(sizeof(payload) - 1));
    char buf[16];
    TEST_ASSERT_EQ(read_or_neg1(f, 0, buf, sizeof(buf)), static_cast<int64_t>(sizeof(payload) - 1));
    TEST_ASSERT_EQ(memcmp(buf, payload, sizeof(payload) - 1), 0);

    // The file is reachable by relative path through the mounted FS too.
    TEST_ASSERT_EQ(lookup_or_null(fs, "gcc_out.o"), f);

    TEST_ASSERT_EQ(do_umount2_kernel(kPathB, 0), 0);
}

void test_mount_unknown_fstype_is_enodev() {
    // ext2 factory (B1b) needs a source path -> EINVAL without one (not ENODEV).
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "ext2", 0), -cinux::kEinval);
    // A nonsense fstype is still ENODEV.
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "nonsense", 0), -cinux::kEnodev);
    TEST_ASSERT_NULL(fs_at(kPathA));  // nothing mounted on failure
}

void test_umount_missing_is_enoent() {
    TEST_ASSERT_EQ(do_umount2_kernel("/tmp_t_never_mounted", 0), -cinux::kEnoent);
    // Empty/null target is EINVAL.
    TEST_ASSERT_EQ(do_umount2_kernel("", 0), -cinux::kEinval);
}

/// Mount, umount, then mount again at the same path: the slot is reused and the
/// first (owned) backend was freed by the umount -- a second mount of the same
/// kind must still succeed and behave like a fresh empty filesystem.
void test_remount_after_umount_is_fresh() {
    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "tmpfs", 0), 0);
    {
        const char* rel  = nullptr;
        auto*       fs   = vfs_resolve(kPathA, &rel);
        auto*       root = lookup_or_null(fs, "");
        create_or_null(root, "stale", 5);  // leave a file behind
    }
    TEST_ASSERT_EQ(do_umount2_kernel(kPathA, 0), 0);  // frees the owned backend + its tree

    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "tmpfs", 0), 0);
    {
        const char* rel  = nullptr;
        auto*       fs   = vfs_resolve(kPathA, &rel);
        auto*       root = lookup_or_null(fs, "");
        TEST_ASSERT_NOT_NULL(root);
        // Fresh mount: the "stale" file from the first mount is gone (the tree
        // was freed on umount, not leaked into the new backend).
        TEST_ASSERT_NULL(lookup_or_null(fs, "stale"));
    }
    TEST_ASSERT_EQ(do_umount2_kernel(kPathA, 0), 0);
}

/// proc factory: the boot singleton is mounted at /proc; sys_mount -t proc at a
/// second path shares it (owned=false).  run-kernel-test's slim boot may skip
/// procfs::init(); mount() is idempotent (it guards on mounted_), so mount the
/// singleton here first when the boot path left it uninitialised.
void test_mount_proc_shares_singleton() {
    if (cinux::fs::procfs::instance() == nullptr) {
        cinux::fs::procfs::init();  // idempotent
    }
    TEST_ASSERT_NOT_NULL(cinux::fs::procfs::instance());

    TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "proc", 0), 0);
    TEST_ASSERT_NOT_NULL(fs_at(kPathA));
    TEST_ASSERT_NOT_NULL(fs_at("/proc"));  // boot mount (from init) still resolves
    TEST_ASSERT_EQ(do_umount2_kernel(kPathA, 0), 0);
    TEST_ASSERT_NULL(fs_at(kPathA));
    TEST_ASSERT_NOT_NULL(fs_at("/proc"));  // singleton survived the umount of kPathA
}

/// devfs factory: same singleton-sharing semantics as proc.  devfs::init() is
/// NOT idempotent (it re-registers nodes), so the factory refuses -ENODEV when
/// the boot singleton is uninitialised rather than expose it; when devfs IS
/// mounted (production boot) it shares the singleton like proc.
void test_mount_devfs_factory() {
    if (cinux::fs::devfs::instance() != nullptr) {
        TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "devfs", 0), 0);
        TEST_ASSERT_NOT_NULL(fs_at(kPathA));
        TEST_ASSERT_NOT_NULL(fs_at("/dev"));  // original /dev still resolves
        TEST_ASSERT_EQ(do_umount2_kernel(kPathA, 0), 0);
        TEST_ASSERT_NULL(fs_at(kPathA));
        TEST_ASSERT_NOT_NULL(fs_at("/dev"));  // singleton survived the umount
    } else {
        // Slim test boot without devfs::init(): factory must refuse rather than
        // mount an uninitialised singleton.
        TEST_ASSERT_EQ(do_mount_kernel(nullptr, kPathA, "devfs", 0), -cinux::kEnodev);
    }
}

/// ext2 factory (B1b): source /dev/sda resolves to the boot-registered block
/// device (main_test wires its AHCI port-1 ext2 disk as "sda"), and sys_mount
/// -t ext2 mounts a fresh Ext2 over it.  Requires the BlockRegistry + the DevFs
/// /dev/sda node, both set up by boot before the test suites run.
void test_mount_ext2_from_block_device() {
    if (cinux::drivers::BlockRegistry::lookup("sda") == nullptr) {
        return;  // no block device registered (slim boot without a disk) -- skip
    }
    TEST_ASSERT_EQ(do_mount_kernel("/dev/sda", kPathB, "ext2", 0), 0);
    const char* rel = nullptr;
    auto*       fs  = vfs_resolve(kPathB, &rel);
    TEST_ASSERT_NOT_NULL(fs);
    auto* root = lookup_or_null(fs, "");
    TEST_ASSERT_NOT_NULL(root);  // Ext2 mounted, root inode resolves
    TEST_ASSERT_EQ(do_umount2_kernel(kPathB, 0), 0);
    TEST_ASSERT_NULL(fs_at(kPathB));
}

}  // namespace test_mount

extern "C" void run_mount_tests() {
    TEST_SECTION("mount/umount2 (F6-M1)");
    RUN_TEST(test_mount::test_mount_tmpfs_resolves);
    RUN_TEST(test_mount::test_mount_then_create_write_read);
    RUN_TEST(test_mount::test_mount_unknown_fstype_is_enodev);
    RUN_TEST(test_mount::test_umount_missing_is_enoent);
    RUN_TEST(test_mount::test_remount_after_umount_is_fresh);
    RUN_TEST(test_mount::test_mount_proc_shares_singleton);
    RUN_TEST(test_mount::test_mount_devfs_factory);
    RUN_TEST(test_mount::test_mount_ext2_from_block_device);
    TEST_SUMMARY();
}
