/**
 * @file kernel/test/test_vfs_syscall.cpp
 * @brief QEMU in-kernel integration tests for the VFS syscall layer
 *
 * Verifies that the sys_open / sys_read / sys_write / sys_close / sys_getdents
 * handlers integrate correctly with the VFS mount table, FDTable, and Ramdisk
 * backend.
 *
 * Test matrix:
 *   - sys_open with valid path returns a non-negative fd
 *   - sys_read through fd returns correct file data and updates offset
 *   - sys_read after sys_close returns -1 (fd no longer valid)
 *   - sys_open with non-existent path returns -1
 *   - sys_open with null / empty path returns -1
 *   - sys_write to a read-only ramdisk file returns -1
 *   - sys_write with invalid fd returns -1
 *   - sys_read with invalid fd returns -1
 *   - sys_close with invalid fd returns -1
 *   - sys_open multiple files, read interleaved, close all
 *   - sys_getdents reads directory entries (".", "..", file names)
 *   - sys_getdents returns 0 when directory is exhausted
 *   - sys_getdents returns -1 on invalid fd / closed fd / null buf / bad addr
 *   - vfs_mount_init + vfs_mount_add registration before syscalls
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (needed for new/delete in FDTable)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/fs/file.hpp"
#include "kernel/fs/ramdisk/ramdisk.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/fs/tmpfs/tmpfs.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_getdents.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_write.hpp"

using cinux::fs::Ramdisk;

// ============================================================
// Helper: set up a fresh VFS + Ramdisk for each test
// ============================================================

namespace {

/// Mount path used by all tests in this file
static constexpr const char* MOUNT_PATH = "/";

/**
 * @brief Initialise VFS, create a Ramdisk, mount it, and return it
 *
 * Each test gets a clean mount table and FD table state.  The caller
 * must call teardown_vfs() when done to avoid leaking the Ramdisk.
 */
Ramdisk* setup_vfs() {
    cinux::fs::vfs_mount_init();

    auto* rd = new Ramdisk();
    ASSERT_OK(rd->mount());

    cinux::fs::vfs_mount_add(MOUNT_PATH, rd);
    return rd;
}

/// Unmount and destroy the Ramdisk created by setup_vfs()
void teardown_vfs(Ramdisk* rd) {
    cinux::fs::vfs_mount_remove(MOUNT_PATH);
    delete rd;
}

cinux::fs::TmpFs* setup_tmpfs_at_tmp() {
    cinux::fs::vfs_mount_init();

    auto* tfs = new cinux::fs::TmpFs();
    ASSERT_OK(tfs->mount());

    if (!cinux::fs::vfs_mount_add("/tmp", tfs)) {
        delete tfs;
        return nullptr;
    }
    return tfs;
}

void teardown_tmpfs_at_tmp(cinux::fs::TmpFs* tfs) {
    cinux::fs::vfs_mount_remove("/tmp");
    delete tfs;
}

}  // anonymous namespace

// ============================================================
// Test 1: vfs_mount_init + vfs_mount_add registration
// ============================================================

namespace test_vfs_mount_reg {

void test_mount_init_add_resolve() {
    cinux::fs::vfs_mount_init();

    auto* rd = new Ramdisk();
    TEST_ASSERT_TRUE(rd->mount().ok());
    TEST_ASSERT_TRUE(cinux::fs::vfs_mount_add(MOUNT_PATH, rd));

    // Resolve should find the ramdisk for any "/"-prefixed path
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve("/hello.txt", &rel_path);
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(rel_path);

    // The Ramdisk should be able to look up the file
    cinux::fs::Inode* inode = lookup_or_null(fs, rel_path);
    TEST_ASSERT_NOT_NULL(inode);

    teardown_vfs(rd);
}

void test_mount_add_null_path_fails() {
    cinux::fs::vfs_mount_init();

    Ramdisk rd;
    ASSERT_OK(rd.mount());

    TEST_ASSERT_FALSE(cinux::fs::vfs_mount_add(nullptr, &rd));
}

void test_mount_add_null_fs_fails() {
    cinux::fs::vfs_mount_init();

    TEST_ASSERT_FALSE(cinux::fs::vfs_mount_add("/test", nullptr));
}

void test_resolve_no_mount_returns_null() {
    cinux::fs::vfs_mount_init();

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve("/anything", &rel_path);
    TEST_ASSERT_NULL(fs);
}

}  // namespace test_vfs_mount_reg

// ============================================================
// Test 2: sys_open -- valid path returns fd
// ============================================================

namespace test_sys_open {

void test_open_valid_path_returns_fd() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";

    int64_t fd = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Clean up
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_open_nonexistent_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/nonexistent.txt";

    int64_t fd = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_LT(fd, 0);

    teardown_vfs(rd);
}

void test_open_null_path_returns_error() {
    Ramdisk* rd = setup_vfs();

    int64_t fd = cinux::syscall::sys_open(0, 0, 0, 0, 0, 0);
    TEST_ASSERT_LT(fd, 0);

    teardown_vfs(rd);
}

void test_open_empty_path_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path      = "";
    auto        path_addr = reinterpret_cast<uint64_t>(path);

    int64_t fd = cinux::syscall::sys_open(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_LT(fd, 0);

    teardown_vfs(rd);
}

void test_open_invalid_addr_returns_error() {
    Ramdisk* rd = setup_vfs();

    // Address above USER_ADDR_MAX should be rejected
    uint64_t bad_addr = 0x800000000001ULL;

    int64_t fd = cinux::syscall::sys_open(bad_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_LT(fd, 0);

    teardown_vfs(rd);
}

void test_open_no_mount_returns_error() {
    // No mount point registered at all
    cinux::fs::vfs_mount_init();

    const char* path = "/hello.txt";

    int64_t fd = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_LT(fd, 0);
}

void test_sys_open_creates_tmpfs_file_with_o_creat() {
    cinux::fs::TmpFs* tfs = setup_tmpfs_at_tmp();
    TEST_ASSERT_NOT_NULL(tfs);

    constexpr uint64_t kOWronly = 0x1;
    constexpr uint64_t kOCreat  = 0x40;
    constexpr uint64_t kOTrunc  = 0x200;
    const char*        path     = "/tmp/cc-test.s";

    int64_t fd = cinux::syscall::do_openat_kernel(path, kOWronly | kOCreat | kOTrunc, 0755);
    TEST_ASSERT_GE(fd, 0);
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    cinux::fs::Inode* inode = lookup_or_null(tfs, "cc-test.s");
    TEST_ASSERT_NOT_NULL(inode);
    TEST_ASSERT_EQ(static_cast<int>(inode->type), static_cast<int>(cinux::fs::InodeType::Regular));
    cinux::fs::stat st;
    TEST_ASSERT_TRUE(inode->ops->stat(inode, &st).ok());
    TEST_ASSERT_EQ(st.st_mode, static_cast<uint32_t>(cinux::fs::kTmpfsSIfReg | 0755));

    teardown_tmpfs_at_tmp(tfs);
}

}  // namespace test_sys_open

// ============================================================
// Test 3: sys_read -- read file content through fd
// ============================================================

namespace test_sys_read {

void test_read_returns_correct_data() {
    Ramdisk* rd = setup_vfs();

    // Open hello.txt
    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Read into a local buffer
    char    buf[64] = {};
    int64_t n       = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf, sizeof(buf) - 1);
    TEST_ASSERT_GT(n, 0);

    const char expected[]   = "Hello from Cinux!\n";
    auto       expected_len = static_cast<uint64_t>(sizeof(expected) - 1);
    TEST_ASSERT_EQ(static_cast<uint64_t>(n), expected_len);
    TEST_ASSERT_TRUE(memcmp(buf, expected, expected_len) == 0);

    // Clean up
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_read_updates_offset() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // First read: 4 bytes
    char    buf1[8] = {};
    int64_t n1      = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf1, 4);
    TEST_ASSERT_EQ(n1, 4);
    TEST_ASSERT_TRUE(memcmp(buf1, "Hell", 4) == 0);

    // Second read: 4 bytes from updated offset
    char    buf2[8] = {};
    int64_t n2      = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf2, 4);
    TEST_ASSERT_EQ(n2, 4);
    TEST_ASSERT_TRUE(memcmp(buf2, "o fr", 4) == 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_read_past_end_returns_zero() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Read all content first
    char    big_buf[128] = {};
    int64_t n1 = cinux::syscall::do_read_kernel(static_cast<int>(fd), big_buf, sizeof(big_buf));
    TEST_ASSERT_GT(n1, 0);

    // Read again -- offset is now past EOF, should return 0
    char    small_buf[8] = {};
    int64_t n2 = cinux::syscall::do_read_kernel(static_cast<int>(fd), small_buf, sizeof(small_buf));
    TEST_ASSERT_EQ(n2, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_read_invalid_fd_returns_error() {
    // P0b: bad fd is kernel logic (do_read_kernel -> -EBADF), not access_ok.
    char    discard[10] = {};
    int64_t n           = cinux::syscall::do_read_kernel(99, discard, 10);
    TEST_ASSERT_LT(n, 0);
}

void test_read_after_close_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Close the fd
    int64_t close_result = cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(close_result, 0);

    // Read from closed fd should return -1
    char    buf[8] = {};
    int64_t n      = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf, sizeof(buf));
    TEST_ASSERT_LT(n, 0);

    teardown_vfs(rd);
}

}  // namespace test_sys_read

// ============================================================
// Test 4: sys_write -- write to ramdisk returns error
// ============================================================

namespace test_sys_write {

void test_write_to_ramdisk_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    const char data[] = "test data";
    int64_t    n = cinux::syscall::do_write_kernel(static_cast<int>(fd), data, sizeof(data) - 1);
    TEST_ASSERT_LT(n, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_write_invalid_fd_returns_error() {
    const char data[] = "test";
    int64_t    n      = cinux::syscall::do_write_kernel(99, data, 4);
    TEST_ASSERT_LT(n, 0);
}

void test_write_after_close_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    const char data[] = "test";
    int64_t    n      = cinux::syscall::do_write_kernel(static_cast<int>(fd), data, 4);
    TEST_ASSERT_LT(n, 0);

    teardown_vfs(rd);
}

}  // namespace test_sys_write

// ============================================================
// Test 5: sys_close -- close fd and verify
// ============================================================

namespace test_sys_close {

void test_close_valid_fd_returns_zero() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    int64_t result = cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);

    // Second close should fail
    int64_t result2 = cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result2, -1);

    teardown_vfs(rd);
}

void test_close_invalid_fd_returns_error() {
    int64_t result = cinux::syscall::sys_close(200, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, -1);
}

}  // namespace test_sys_close

// ============================================================
// Test 6: Full open-read-write-close lifecycle
// ============================================================

namespace test_vfs_lifecycle {

void test_open_read_close_lifecycle() {
    Ramdisk* rd = setup_vfs();

    // Open hello.txt
    const char* path = "/hello.txt";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Read the file content
    char    buf[64] = {};
    int64_t n       = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf, sizeof(buf) - 1);
    TEST_ASSERT_GT(n, 0);

    const char expected[] = "Hello from Cinux!\n";
    TEST_ASSERT_TRUE(memcmp(buf, expected, sizeof(expected) - 1) == 0);

    // Write should fail (read-only ramdisk)
    const char data[] = "cannot write";
    int64_t write_n = cinux::syscall::do_write_kernel(static_cast<int>(fd), data, sizeof(data) - 1);
    TEST_ASSERT_LT(write_n, 0);

    // Close the fd
    int64_t close_result = cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(close_result, 0);

    // Read after close should fail
    char    buf2[8] = {};
    int64_t n2      = cinux::syscall::do_read_kernel(static_cast<int>(fd), buf2, sizeof(buf2));
    TEST_ASSERT_LT(n2, 0);

    teardown_vfs(rd);
}

void test_open_multiple_files_interleaved() {
    Ramdisk* rd = setup_vfs();

    // Open hello.txt
    const char* path1 = "/hello.txt";
    int64_t     fd1   = cinux::syscall::do_open_kernel(path1, 0);
    TEST_ASSERT_GE(fd1, 0);

    // Open readme.txt
    const char* path2 = "/readme.txt";
    int64_t     fd2   = cinux::syscall::do_open_kernel(path2, 0);
    TEST_ASSERT_GE(fd2, 0);
    TEST_ASSERT_NE(fd1, fd2);

    // Read from hello.txt
    char    buf1[64] = {};
    int64_t n1 = cinux::syscall::do_read_kernel(static_cast<int>(fd1), buf1, sizeof(buf1) - 1);
    TEST_ASSERT_GT(n1, 0);

    // Verify hello.txt content
    const char expected1[] = "Hello from Cinux!\n";
    TEST_ASSERT_TRUE(memcmp(buf1, expected1, sizeof(expected1) - 1) == 0);

    // Read from readme.txt
    char    buf2[64] = {};
    int64_t n2 = cinux::syscall::do_read_kernel(static_cast<int>(fd2), buf2, sizeof(buf2) - 1);
    TEST_ASSERT_GT(n2, 0);

    // Close both
    TEST_ASSERT_EQ(cinux::syscall::sys_close(static_cast<uint64_t>(fd1), 0, 0, 0, 0, 0), 0);
    TEST_ASSERT_EQ(cinux::syscall::sys_close(static_cast<uint64_t>(fd2), 0, 0, 0, 0, 0), 0);

    // Both fds should now be invalid
    TEST_ASSERT_LT(cinux::syscall::do_read_kernel(static_cast<int>(fd1), buf1, 4), 0);
    TEST_ASSERT_LT(cinux::syscall::do_read_kernel(static_cast<int>(fd2), buf2, 4), 0);

    teardown_vfs(rd);
}

}  // namespace test_vfs_lifecycle

// ============================================================
// Test 7: sys_getdents -- read directory entries
// ============================================================

namespace test_sys_getdents {

void test_getdents_reads_entries_in_order() {
    Ramdisk* rd = setup_vfs();

    // Open the root directory
    const char* path = "/";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    char buf[128] = {};

    // Entry 0: "."
    int64_t n0 = cinux::syscall::do_getdents_kernel(static_cast<int>(fd), buf, sizeof(buf));
    TEST_ASSERT_GT(n0, 0);
    TEST_ASSERT_TRUE(memcmp(buf, ".", 2) == 0);

    // Entry 1: ".."
    n0 = cinux::syscall::do_getdents_kernel(static_cast<int>(fd), buf, sizeof(buf));
    TEST_ASSERT_GT(n0, 0);
    TEST_ASSERT_TRUE(memcmp(buf, "..", 3) == 0);

    // Entry 2+: actual files from the ramdisk -- at least one must exist
    n0 = cinux::syscall::do_getdents_kernel(static_cast<int>(fd), buf, sizeof(buf));
    TEST_ASSERT_GT(n0, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_getdents_returns_zero_when_exhausted() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    char buf[128] = {};

    // Drain all entries ("." , "..", and all ramdisk files)
    for (int i = 0; i < 128; ++i) {
        int64_t n = cinux::syscall::do_getdents_kernel(static_cast<int>(fd), buf, sizeof(buf));
        if (n == 0) {
            // Reached end of directory -- success
            cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
            teardown_vfs(rd);
            return;
        }
        TEST_ASSERT_GT(n, 0);
    }

    // If we got here, 128 entries without hitting end-of-dir is suspicious
    TEST_ASSERT_TRUE(false);
}

void test_getdents_invalid_fd_returns_error() {
    char    buf[64] = {};
    int64_t n       = cinux::syscall::do_getdents_kernel(99, buf, sizeof(buf));
    TEST_ASSERT_LT(n, 0);
}

void test_getdents_after_close_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    char    buf[64] = {};
    int64_t n       = cinux::syscall::do_getdents_kernel(static_cast<int>(fd), buf, sizeof(buf));
    TEST_ASSERT_LT(n, 0);

    teardown_vfs(rd);
}

void test_getdents_null_buf_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // buf_virt == 0 should be rejected
    int64_t n = cinux::syscall::sys_getdents(static_cast<uint64_t>(fd), 0, 64, 0, 0, 0);
    TEST_ASSERT_LT(n, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

void test_getdents_noncanonical_addr_returns_error() {
    Ramdisk* rd = setup_vfs();

    const char* path = "/";
    int64_t     fd   = cinux::syscall::do_open_kernel(path, 0);
    TEST_ASSERT_GE(fd, 0);

    // Address above USER_ADDR_MAX should be rejected
    uint64_t bad_addr = 0x800000000001ULL;
    int64_t  n = cinux::syscall::sys_getdents(static_cast<uint64_t>(fd), bad_addr, 64, 0, 0, 0);
    TEST_ASSERT_LT(n, 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    teardown_vfs(rd);
}

}  // namespace test_sys_getdents

// ============================================================
// Entry point
// ============================================================

extern "C" void run_vfs_syscall_tests() {
    TEST_SECTION("VFS Syscall Integration Tests (027)");

    // Mount registration
    RUN_TEST(test_vfs_mount_reg::test_mount_init_add_resolve);
    RUN_TEST(test_vfs_mount_reg::test_mount_add_null_path_fails);
    RUN_TEST(test_vfs_mount_reg::test_mount_add_null_fs_fails);
    RUN_TEST(test_vfs_mount_reg::test_resolve_no_mount_returns_null);

    // sys_open
    RUN_TEST(test_sys_open::test_open_valid_path_returns_fd);
    RUN_TEST(test_sys_open::test_open_nonexistent_returns_error);
    RUN_TEST(test_sys_open::test_open_null_path_returns_error);
    RUN_TEST(test_sys_open::test_open_empty_path_returns_error);
    RUN_TEST(test_sys_open::test_open_invalid_addr_returns_error);
    RUN_TEST(test_sys_open::test_open_no_mount_returns_error);
    RUN_TEST(test_sys_open::test_sys_open_creates_tmpfs_file_with_o_creat);

    // sys_read
    RUN_TEST(test_sys_read::test_read_returns_correct_data);
    RUN_TEST(test_sys_read::test_read_updates_offset);
    RUN_TEST(test_sys_read::test_read_past_end_returns_zero);
    RUN_TEST(test_sys_read::test_read_invalid_fd_returns_error);
    RUN_TEST(test_sys_read::test_read_after_close_returns_error);

    // sys_write
    RUN_TEST(test_sys_write::test_write_to_ramdisk_returns_error);
    RUN_TEST(test_sys_write::test_write_invalid_fd_returns_error);
    RUN_TEST(test_sys_write::test_write_after_close_returns_error);

    // sys_close
    RUN_TEST(test_sys_close::test_close_valid_fd_returns_zero);
    RUN_TEST(test_sys_close::test_close_invalid_fd_returns_error);

    // Full lifecycle
    RUN_TEST(test_vfs_lifecycle::test_open_read_close_lifecycle);
    RUN_TEST(test_vfs_lifecycle::test_open_multiple_files_interleaved);

    // sys_getdents
    RUN_TEST(test_sys_getdents::test_getdents_reads_entries_in_order);
    RUN_TEST(test_sys_getdents::test_getdents_returns_zero_when_exhausted);
    RUN_TEST(test_sys_getdents::test_getdents_invalid_fd_returns_error);
    RUN_TEST(test_sys_getdents::test_getdents_after_close_returns_error);
    RUN_TEST(test_sys_getdents::test_getdents_null_buf_returns_error);
    RUN_TEST(test_sys_getdents::test_getdents_noncanonical_addr_returns_error);

    TEST_SUMMARY();
}
