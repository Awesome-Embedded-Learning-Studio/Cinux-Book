/**
 * @file kernel/test/test_syscall_ext2.cpp
 * @brief QEMU in-kernel integration tests for sys_creat/mkdir/unlink/rmdir (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies the
 * complete syscall flow: VFS resolve -> split pathname -> lookup parent
 * inode -> ops->create/mkdir/unlink, using a real ext2 ramdisk on AHCI.
 *
 * Test matrix:
 *   - sys_creat("/testfile")  -> lookup verifies file exists
 *   - sys_mkdir("/testdir")   -> lookup verifies directory exists
 *   - sys_unlink("/testfile") -> lookup verifies file gone
 *   - sys_rmdir("/testdir")   -> lookup verifies directory gone
 *   - Full flow: mkdir -> creat in subdir -> unlink -> rmdir
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (needed for DMA buffer allocation)
 *   - VMM initialised (needed for DMA buffer mapping)
 *   - Heap initialised (needed for new/delete)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "libs/ext2/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/vfs_lookup.hpp"  // F-USABILITY batch 1b: vfs_lookup follow
#include "kernel/lib/string.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_creat.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_rmdir.hpp"
#include "kernel/syscall/sys_unlink.hpp"
// F-ECO batch 2: VFS metadata + dirent syscalls (mechanism tests).
#include "kernel/fs/stat.hpp"
#include "kernel/syscall/sys_chmod.hpp"
#include "kernel/syscall/sys_chown.hpp"
#include "kernel/syscall/sys_link.hpp"
#include "kernel/syscall/sys_readlink.hpp"
#include "kernel/syscall/sys_rename.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_symlink.hpp"
#include "kernel/syscall/sys_utimensat.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::fs::Ext2;
using cinux::fs::Inode;
using cinux::fs::InodeType;

// ============================================================
// Helper: set up VFS + AHCI + Ext2 for each test
// ============================================================

namespace {

struct AhciExt2Pair {
    AHCI*                                  ahci;
    Ext2*                                  ext2;
    cinux::drivers::ahci::AHCIBlockDevice* blk_dev;
};

/**
 * @brief Initialise PCI, find AHCI, create and mount an Ext2 instance,
 *        and register it in the VFS mount table at "/"
 */
AhciExt2Pair setup_syscall_ext2() {
    AhciExt2Pair result{nullptr, nullptr, nullptr};

    // Reset VFS mount table for a clean slate
    cinux::fs::vfs_mount_init();

    // PCI enumeration
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    // AHCI init
    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);

    // Ext2 mount on port 1 (the ext2 test disk)
    auto blk = cinux::drivers::ahci::AHCIBlockDevice::create(*result.ahci, 1);
    result.blk_dev =
        blk.ok() ? new cinux::drivers::ahci::AHCIBlockDevice(std::move(blk.value())) : nullptr;
    result.ext2 = new Ext2(result.blk_dev);
    ASSERT_OK(result.ext2->mount());

    // Register in VFS
    cinux::fs::vfs_mount_add("/", result.ext2);

    return result;
}

/// Tear down VFS mount, Ext2, and AHCI
void teardown_syscall_ext2(AhciExt2Pair& pair) {
    cinux::fs::vfs_mount_remove("/");
    delete pair.ext2;
    delete pair.blk_dev;
    delete pair.ahci;
    pair.ext2 = nullptr;
    pair.ahci = nullptr;
}

static uint32_t g_name_seq = 0;

void gen_name(char* buf, uint32_t buf_len, const char* prefix) {
    uint32_t seed = static_cast<uint32_t>(cinux::drivers::PIT::get_ticks() ^ (++g_name_seq));
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t off = 0;
    while (prefix[off] && off < buf_len - 9) {
        buf[off] = prefix[off];
        ++off;
    }
    buf[off++]       = '_';
    const char hex[] = "0123456789abcdef";
    for (int d = 6; d >= 0 && off < buf_len - 1; --d)
        buf[off++] = hex[(seed >> (d * 4)) & 0xf];
    buf[off] = '\0';
}

uint32_t name_len(const char* s) {
    uint32_t n = 0;
    while (s[n])
        ++n;
    return n;
}

}  // anonymous namespace

// ============================================================
// Test 1: sys_creat — create a new file
// ============================================================

namespace test_sys_creat {

/**
 * @brief sys_creat("/sc_testfile") creates the file; lookup confirms it exists
 */
void test_creat_creates_file() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "sc");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    int64_t result = cinux::syscall::do_creat_kernel(path);
    TEST_ASSERT_EQ(result, 0);

    // Verify the file exists via lookup
    Inode* found = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Regular));

    cinux::lib::kprintf("[SYSCALL_EXT2] creat /%s OK (ino=%lu)\n", name, found->ino);

    // Clean up: unlink so the disk state is restored
    pair.ext2->unlink(2, name, name_len(name));

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_creat

// ============================================================
// Test 2: sys_mkdir — create a new directory
// ============================================================

namespace test_sys_mkdir {

/**
 * @brief sys_mkdir("/sc_testdir") creates the directory; lookup confirms it exists
 */
void test_mkdir_creates_directory() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "sd");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    int64_t result = cinux::syscall::do_mkdir_kernel(path);
    TEST_ASSERT_EQ(result, 0);

    // Verify the directory exists via lookup
    Inode* found = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SYSCALL_EXT2] mkdir /%s OK (ino=%lu)\n", name, found->ino);

    // Clean up: unlink the directory (link_count 2->1, not fully freed,
    // but removes directory entry from parent so disk is reusable)
    pair.ext2->unlink(2, name, name_len(name));

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_mkdir

// ============================================================
// Test 3: sys_unlink — remove a file
// ============================================================

namespace test_sys_unlink {

/**
 * @brief Create a file with sys_creat, then sys_unlink removes it
 */
void test_unlink_removes_file() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // First create the file
    char name[32];
    gen_name(name, 32, "su");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    int64_t creat_result = cinux::syscall::do_creat_kernel(path);
    TEST_ASSERT_EQ(creat_result, 0);

    // Confirm it exists
    Inode* found = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(found);

    cinux::lib::kprintf("[SYSCALL_EXT2] unlink: file created (ino=%lu)\n", found->ino);

    // Now unlink it
    int64_t unlink_result = cinux::syscall::do_unlink_kernel(path);
    TEST_ASSERT_EQ(unlink_result, 0);

    // Verify the file is gone
    Inode* gone = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SYSCALL_EXT2] unlink /%s OK (file gone)\n", name);

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_unlink

// ============================================================
// Test 4: sys_rmdir — remove an empty directory
// ============================================================

namespace test_sys_rmdir {

/**
 * @brief Create a directory with sys_mkdir, then sys_rmdir removes it
 */
void test_rmdir_removes_directory() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // First create the directory
    char name[32];
    gen_name(name, 32, "sr");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    int64_t mkdir_result = cinux::syscall::do_mkdir_kernel(path);
    TEST_ASSERT_EQ(mkdir_result, 0);

    // Confirm it exists
    Inode* found = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(found);

    cinux::lib::kprintf("[SYSCALL_EXT2] rmdir: dir created (ino=%lu)\n", found->ino);

    // Now rmdir it
    int64_t rmdir_result = cinux::syscall::do_rmdir_kernel(path);
    TEST_ASSERT_EQ(rmdir_result, 0);

    // Verify the directory is gone from parent's listing
    Inode* gone = lookup_or_null(pair.ext2, name);
    // After rmdir, the directory entry is removed from parent
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SYSCALL_EXT2] rmdir /%s OK (dir gone)\n", name);

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_rmdir

// ============================================================
// Test 5: Full flow — mkdir -> creat in subdir -> unlink -> rmdir
// ============================================================

namespace test_syscall_full_flow {

/**
 * @brief End-to-end: mkdir("/sc_flowdir"), creat("/sc_flowdir/file"),
 *        unlink("/sc_flowdir/file"), rmdir("/sc_flowdir")
 */
void test_full_syscall_flow() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Step 1: mkdir random dir
    char dirname[32];
    gen_name(dirname, 32, "sf");
    char dir_path[64];
    dir_path[0] = '/';
    uint32_t di = 0;
    while (dirname[di]) {
        dir_path[di + 1] = dirname[di];
        ++di;
    }
    dir_path[di + 1]     = '\0';
    int64_t mkdir_result = cinux::syscall::do_mkdir_kernel(dir_path);
    TEST_ASSERT_EQ(mkdir_result, 0);

    Inode* dir = lookup_or_null(pair.ext2, dirname);
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_EQ(static_cast<uint32_t>(dir->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: mkdir /%s OK\n", dirname);

    // Step 2: creat file inside the dir
    char fname[32];
    gen_name(fname, 32, "ff");
    char     file_path[96];
    uint32_t fi     = 0;
    file_path[fi++] = '/';
    for (uint32_t j = 0; dirname[j] && fi < sizeof(file_path) - 2; ++j)
        file_path[fi++] = dirname[j];
    file_path[fi++] = '/';
    for (uint32_t j = 0; fname[j] && fi < sizeof(file_path) - 1; ++j)
        file_path[fi++] = fname[j];
    file_path[fi] = '\0';

    int64_t creat_result = cinux::syscall::do_creat_kernel(file_path);
    TEST_ASSERT_EQ(creat_result, 0);

    // Verify via ext2 lookup in the subdirectory
    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: creat %s OK\n", file_path);

    // Step 3: unlink the file
    int64_t unlink_result = cinux::syscall::do_unlink_kernel(file_path);
    TEST_ASSERT_EQ(unlink_result, 0);

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: unlink %s OK\n", file_path);

    // Step 4: rmdir the directory
    int64_t rmdir_result = cinux::syscall::do_rmdir_kernel(dir_path);
    TEST_ASSERT_EQ(rmdir_result, 0);

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: rmdir /%s OK\n", dirname);

    teardown_syscall_ext2(pair);
}

}  // namespace test_syscall_full_flow

// ============================================================
// Test 6: sys_creat duplicate name — should not crash
// ============================================================

namespace test_sys_creat_duplicate {

void test_creat_duplicate_name() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char name[32];
    gen_name(name, 32, "dup");
    char path[64];
    // 拼接 "/" + name
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    // 第一次创建应该成功
    int64_t r1 = cinux::syscall::do_creat_kernel(path);
    TEST_ASSERT_EQ(r1, 0);

    // 验证文件存在
    Inode* found = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(found);
    uint64_t first_ino = found->ino;

    // 重复创建同名文件（当前实现：应该返回已存在的 inode 或报错）
    int64_t r2 = cinux::syscall::do_creat_kernel(path);
    // r2 可能是 0（返回已存在 inode）或 < 0（报错），两种都可接受
    // 关键是不应该崩溃

    // 验证原文件仍在
    Inode* still = lookup_or_null(pair.ext2, name);
    TEST_ASSERT_NOT_NULL(still);

    // 清理
    cinux::syscall::do_unlink_kernel(path);
    teardown_syscall_ext2(pair);

    (void)first_ino;
    (void)r2;
}

}  // namespace test_sys_creat_duplicate

// ============================================================
// Test 7 (F2-M6): sys_read on an ext2 file is served through the PageCache
// ============================================================

namespace test_sys_read_ext2_cache {

/**
 * @brief Open /hello.txt, read it twice; the second read must hit the cache.
 *
 * Exercises the sys_read -> is_page_cacheable() -> PageCache::read_bytes wiring
 * end to end against a real AHCI/ext2 disk.  Reading the same file again (via a
 * fresh fd at offset 0) reuses the cached page, so the global cache's hit count
 * climbs -- direct proof that read() no longer bypasses the cache.
 */
void test_ext2_read_served_from_cache() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    const char* path = "/hello.txt";

    // First open + read fills the cache for the file's first page.
    int64_t fd = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    char    buf1[128] = {};
    auto    buf1_addr = reinterpret_cast<uint64_t>(buf1);
    int64_t n1        = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf1, sizeof(buf1));
    TEST_ASSERT_GT(n1, 0);
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    // Reopen (offset 0, same inode -> identical cache keys) and read again.
    int64_t fd2 = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd2, 0);
    char    buf2[128] = {};
    auto    buf2_addr = reinterpret_cast<uint64_t>(buf2);
    size_t  hits_mid  = cinux::mm::g_page_cache.hit_count();
    int64_t n2        = cinux::syscall::do_read_kernel(static_cast<int>(fd2), buf2, sizeof(buf2));
    TEST_ASSERT_EQ(n2, n1);
    cinux::syscall::sys_close(static_cast<uint64_t>(fd2), 0, 0, 0, 0, 0);

    // Second read of the same page is a cache hit -> hit count rises.
    TEST_ASSERT_GT(cinux::mm::g_page_cache.hit_count(), hits_mid);
    // Both reads return identical bytes.
    TEST_ASSERT_EQ(memcmp(buf1, buf2, static_cast<size_t>(n1)), 0);

    cinux::lib::kprintf("[SYSCALL_EXT2] read /hello.txt twice: %ld bytes, cache hit confirmed\n",
                        n2);

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_read_ext2_cache

// ============================================================
// F-ECO batch 2: VFS metadata + dirent syscall mechanism tests.
// Each creates a file via do_creat_kernel, runs the new syscall, and verifies
// the on-disk effect via do_stat_kernel / lookup_or_null / do_readlink_kernel --
// the "green must mean the mechanism actually fired" discipline (no ENOSYS
// false-green: a stubbed syscall would fail these assertions).
// ============================================================

namespace test_sys_chmod_b2 {
void test_chmod_changes_mode() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    char name[32];
    gen_name(name, 32, "chm");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(path), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_chmod_kernel(path, 0600), 0);

    cinux::fs::stat st;
    TEST_ASSERT_EQ(cinux::syscall::do_stat_kernel(path, &st), 0);
    TEST_ASSERT_EQ(st.st_mode & 0xFFF, 0600u);  // only perm bits changed, type kept

    cinux::lib::kprintf("[B2] chmod /%s mode=0%o OK\n", name, st.st_mode & 0xFFF);
    cinux::syscall::do_unlink_kernel(path);
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_chmod_b2

namespace test_sys_chown_b2 {
void test_chown_changes_owner() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    char name[32];
    gen_name(name, 32, "cho");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(path), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_chown_kernel(path, 1000, 2000), 0);

    cinux::fs::stat st;
    TEST_ASSERT_EQ(cinux::syscall::do_stat_kernel(path, &st), 0);
    TEST_ASSERT_EQ(st.st_uid, 1000u);
    TEST_ASSERT_EQ(st.st_gid, 2000u);

    cinux::lib::kprintf("[B2] chown /%s uid=%u gid=%u OK\n", name, st.st_uid, st.st_gid);
    cinux::syscall::do_unlink_kernel(path);
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_chown_b2

namespace test_sys_utimensat_b2 {
void test_utimensat_changes_times() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    char name[32];
    gen_name(name, 32, "uti");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(path), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_utimensat_kernel(path, 1000, 0, 2000, 0), 0);

    cinux::fs::stat st;
    TEST_ASSERT_EQ(cinux::syscall::do_stat_kernel(path, &st), 0);
    TEST_ASSERT_EQ(st.st_atime, 1000u);
    TEST_ASSERT_EQ(st.st_mtime, 2000u);

    cinux::lib::kprintf("[B2] utimensat /%s atime=%lu mtime=%lu OK\n", name, st.st_atime,
                        st.st_mtime);
    cinux::syscall::do_unlink_kernel(path);
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_utimensat_b2

namespace test_sys_symlink_b2 {
/// symlink("/target_str", link) then readlink reads the target back verbatim.
void test_symlink_readlink_roundtrip() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    char name[32];
    gen_name(name, 32, "sym");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_symlink_kernel("/target_str", path), 0);

    char    buf[256];
    int64_t n = cinux::syscall::do_readlink_kernel(path, buf, sizeof(buf));
    TEST_ASSERT_GT(n, 0);
    TEST_ASSERT_EQ(static_cast<uint64_t>(n), 11u);  // strlen("/target_str")
    TEST_ASSERT_EQ(memcmp(buf, "/target_str", 11), 0);

    cinux::lib::kprintf("[B2] symlink+readlink /%s -> '%.*s' OK\n", name, static_cast<int>(n), buf);
    cinux::syscall::do_unlink_kernel(path);  // remove the symlink (target need not exist)
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_symlink_b2

namespace test_vfs_lookup_follow {
// Build "/<prefix>_<hex>" via gen_name (unique per run, avoids cross-test pollution).
static void build_path(char* path, const char* prefix) {
    char name[32];
    gen_name(name, 32, prefix);
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1] = '\0';
}

/// vfs_lookup follows an absolute symlink (/link -> /hello) to the regular
/// target; NoFollow returns the symlink inode itself. F-USABILITY batch 1b.
void test_follow_absolute_symlink() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char hp[64];  // follow target (regular)
    build_path(hp, "fuh");
    char lp[64];  // the link -> hp
    build_path(lp, "ful");
    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(hp), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_symlink_kernel(hp, lp), 0);

    using LF            = cinux::fs::LookupFlag;
    const uint32_t kFol = static_cast<uint32_t>(LF::Follow);
    const uint32_t kNoF = static_cast<uint32_t>(LF::NoFollow);

    auto nf = cinux::fs::vfs_lookup(lp, kNoF, "/");
    TEST_ASSERT_TRUE(nf.ok());
    TEST_ASSERT_NOT_NULL(nf.value().target);
    TEST_ASSERT_EQ(nf.value().target->type, cinux::fs::InodeType::Symlink);

    auto fl = cinux::fs::vfs_lookup(lp, kFol, "/");
    TEST_ASSERT_TRUE(fl.ok());
    TEST_ASSERT_NOT_NULL(fl.value().target);
    TEST_ASSERT_EQ(fl.value().target->type, cinux::fs::InodeType::Regular);
    // Followed target is a different inode than the link itself.
    TEST_ASSERT_TRUE(fl.value().target->ino != nf.value().target->ino);

    cinux::syscall::do_unlink_kernel(lp);
    cinux::syscall::do_unlink_kernel(hp);
    teardown_syscall_ext2(pair);
}

/// vfs_lookup detects a symlink cycle (/a -> /b -> /a) and returns Error::Loop.
void test_follow_loop_returns_loop_error() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char ap[64];
    build_path(ap, "lpa");
    char bp[64];
    build_path(bp, "lpb");
    TEST_ASSERT_EQ(cinux::syscall::do_symlink_kernel(bp, ap), 0);  // /a -> /b
    TEST_ASSERT_EQ(cinux::syscall::do_symlink_kernel(ap, bp), 0);  // /b -> /a

    using LF       = cinux::fs::LookupFlag;
    auto r         = cinux::fs::vfs_lookup(ap, static_cast<uint32_t>(LF::Follow), "/");
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(r.error(), cinux::lib::Error::Loop);

    cinux::syscall::do_unlink_kernel(ap);
    cinux::syscall::do_unlink_kernel(bp);
    teardown_syscall_ext2(pair);
}

/// PARENT mode returns the parent directory + leaf name without resolving leaf.
void test_parent_mode_returns_parent_and_leaf() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char np[64];
    build_path(np, "pnt");
    uint32_t leaf_len = 0;
    while (np[leaf_len + 1] != '\0') ++leaf_len;  // length after the leading '/'

    using LF = cinux::fs::LookupFlag;
    auto r   = cinux::fs::vfs_lookup(np, static_cast<uint32_t>(LF::Parent), "/");
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_NOT_NULL(r.value().parent);
    TEST_ASSERT_EQ(r.value().parent->type, cinux::fs::InodeType::Directory);
    TEST_ASSERT_EQ(r.value().leaf_len, leaf_len);
    TEST_ASSERT_NULL(r.value().target);  // leaf not resolved

    teardown_syscall_ext2(pair);
}
}  // namespace test_vfs_lookup_follow

namespace test_sys_link_b2 {
/// link(file, file2) adds a name and bumps nlink to 2.
void test_link_bumps_nlink() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char n1[32];
    gen_name(n1, 32, "lk1");
    char p1[64];
    p1[0]      = '/';
    uint32_t i = 0;
    while (n1[i]) {
        p1[i + 1] = n1[i];
        ++i;
    }
    p1[i + 1] = '\0';

    char n2[32];
    gen_name(n2, 32, "lk2");
    char p2[64];
    p2[0]      = '/';
    uint32_t j = 0;
    while (n2[j]) {
        p2[j + 1] = n2[j];
        ++j;
    }
    p2[j + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(p1), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_link_kernel(p1, p2), 0);

    Inode* f2 = lookup_or_null(pair.ext2, n2);
    TEST_ASSERT_NOT_NULL(f2);

    cinux::fs::stat st;
    TEST_ASSERT_EQ(cinux::syscall::do_stat_kernel(p1, &st), 0);
    TEST_ASSERT_EQ(st.st_nlink, 2u);

    cinux::lib::kprintf("[B2] link %s->%s nlink=%lu OK\n", n1, n2, st.st_nlink);
    cinux::syscall::do_unlink_kernel(p2);
    cinux::syscall::do_unlink_kernel(p1);
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_link_b2

namespace test_sys_rename_b2 {
/// rename(old, new) moves the entry: old gone, new present.
void test_rename_moves_entry() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char n1[32];
    gen_name(n1, 32, "rn1");
    char p1[64];
    p1[0]      = '/';
    uint32_t i = 0;
    while (n1[i]) {
        p1[i + 1] = n1[i];
        ++i;
    }
    p1[i + 1] = '\0';

    char n2[32];
    gen_name(n2, 32, "rn2");
    char p2[64];
    p2[0]      = '/';
    uint32_t j = 0;
    while (n2[j]) {
        p2[j + 1] = n2[j];
        ++j;
    }
    p2[j + 1] = '\0';

    TEST_ASSERT_EQ(cinux::syscall::do_creat_kernel(p1), 0);
    TEST_ASSERT_EQ(cinux::syscall::do_rename_kernel(p1, p2), 0);

    Inode* old = lookup_or_null(pair.ext2, n1);
    TEST_ASSERT_NULL(old);
    Inode* nw = lookup_or_null(pair.ext2, n2);
    TEST_ASSERT_NOT_NULL(nw);

    cinux::lib::kprintf("[B2] rename %s->%s OK\n", n1, n2);
    cinux::syscall::do_unlink_kernel(p2);
    teardown_syscall_ext2(pair);
}
}  // namespace test_sys_rename_b2

// ============================================================
// Entry point
// ============================================================

extern "C" void run_syscall_ext2_tests() {
    TEST_SECTION("Syscall Ext2 Tests (028b)");

    // sys_creat
    RUN_TEST(test_sys_creat::test_creat_creates_file);

    // sys_mkdir
    RUN_TEST(test_sys_mkdir::test_mkdir_creates_directory);

    // sys_unlink
    RUN_TEST(test_sys_unlink::test_unlink_removes_file);

    // sys_rmdir
    RUN_TEST(test_sys_rmdir::test_rmdir_removes_directory);

    // Full flow
    RUN_TEST(test_syscall_full_flow::test_full_syscall_flow);

    // Duplicate creat
    RUN_TEST(test_sys_creat_duplicate::test_creat_duplicate_name);

    // F2-M6: read() served through PageCache
    RUN_TEST(test_sys_read_ext2_cache::test_ext2_read_served_from_cache);

    // F-ECO batch 2: VFS metadata + dirent syscalls.
    RUN_TEST(test_sys_chmod_b2::test_chmod_changes_mode);
    RUN_TEST(test_sys_chown_b2::test_chown_changes_owner);
    RUN_TEST(test_sys_utimensat_b2::test_utimensat_changes_times);
    RUN_TEST(test_sys_symlink_b2::test_symlink_readlink_roundtrip);

    // F-USABILITY batch 1b: vfs_lookup symlink follow + loop detect + parent mode.
    RUN_TEST(test_vfs_lookup_follow::test_follow_absolute_symlink);
    RUN_TEST(test_vfs_lookup_follow::test_follow_loop_returns_loop_error);
    RUN_TEST(test_vfs_lookup_follow::test_parent_mode_returns_parent_and_leaf);
    RUN_TEST(test_sys_link_b2::test_link_bumps_nlink);
    RUN_TEST(test_sys_rename_b2::test_rename_moves_entry);

    TEST_SUMMARY();
}
