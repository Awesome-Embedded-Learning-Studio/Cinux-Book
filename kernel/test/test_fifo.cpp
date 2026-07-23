/**
 * @file kernel/test/test_fifo.cpp
 * @brief QEMU in-kernel integration test for named FIFO (F8-M2 batch 5)
 *
 * Drives the full FIFO path through the real kernel machinery: do_mknod_kernel
 * (the sys_mknod body) registers a name; FifoRegistry resolves it; FifoOps::open
 * cloning builds the shared pipe and hands back the read/write ends; the ends
 * are installed as fds and exercised with do_write_kernel / do_read_kernel.
 *
 * (The test kernel does not mount /dev globally, so this reaches the registry
 * directly rather than via VFS path resolution -- the DevFS dynamic-lookup glue
 * is covered by the host test_fifo and is a 4-line fallthrough.)
 *
 * Preconditions (set up by main_test.cpp): serial, GDT/IDT, heap, fd table.
 */

#include "big_kernel_test.h"
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"  // g_global_fd_table
#include "kernel/ipc/fifo.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/syscall/sys_mknod.hpp"  // do_mknod_kernel
#include "kernel/syscall/sys_read.hpp"   // do_read_kernel
#include "kernel/syscall/sys_write.hpp"  // do_write_kernel

using cinux::fs::OpenFlags;

// ============================================================
// 1. mkfifo -> open both ends -> write/read round-trip (headline)
// ============================================================

void test_fifo_mkfifo_open_roundtrip() {
    // Clean slate (idempotent): mkfifo via the syscall layer registers "kfifo".
    cinux::ipc::FifoRegistry::instance().remove("kfifo");
    int64_t mk = cinux::syscall::do_mknod_kernel("/dev/kfifo", cinux::ipc::kSIfFifo | 0666);
    TEST_ASSERT_EQ(mk, 0);

    // Resolve the stable FIFO inode (what DevFS dynamic-lookup returns at runtime).
    auto ino = cinux::ipc::FifoRegistry::instance().lookup_inode("kfifo");
    TEST_ASSERT_TRUE(ino.ok());
    cinux::fs::Inode* fifo_inode = ino.value();

    // Cloning open: O_WRONLY=1 -> write end, O_RDONLY=0 -> read end (shared pipe).
    auto wopen = cinux::ipc::fifo_ops().open(fifo_inode, 1);
    auto ropen = cinux::ipc::fifo_ops().open(fifo_inode, 0);
    TEST_ASSERT_TRUE(wopen.ok());
    TEST_ASSERT_TRUE(ropen.ok());

    // Install the per-open ends as fds in the global table.
    int wfd = cinux::fs::g_global_fd_table().alloc(wopen.value(), OpenFlags::WRONLY);
    int rfd = cinux::fs::g_global_fd_table().alloc(ropen.value(), OpenFlags::RDONLY);
    TEST_ASSERT_GE(wfd, 0);
    TEST_ASSERT_GE(rfd, 0);

    // write -> read across the two fds (exercises PipeWriteOps/PipeReadOps/Pipe).
    const char msg[] = "fifort";
    TEST_ASSERT_EQ(cinux::syscall::do_write_kernel(wfd, msg, 6), 6);

    char    buf[8] = {};
    int64_t r      = cinux::syscall::do_read_kernel(rfd, buf, 6);
    TEST_ASSERT_EQ(r, 6);
    TEST_ASSERT_EQ(buf[0], 'f');
    TEST_ASSERT_EQ(buf[1], 'i');
    TEST_ASSERT_EQ(buf[2], 'f');
    TEST_ASSERT_EQ(buf[3], 'o');
    TEST_ASSERT_EQ(buf[4], 'r');
    TEST_ASSERT_EQ(buf[5], 't');

    cinux::fs::g_global_fd_table().close(wfd);
    cinux::fs::g_global_fd_table().close(rfd);
    cinux::ipc::FifoRegistry::instance().remove("kfifo");
}

// ============================================================
// 2. mknod of a non-FIFO node type is rejected (ENOSYS this milestone)
// ============================================================

void test_fifo_mknod_non_fifo_rejected() {
    // S_IFCHR (0x2000) is not a FIFO -> -ENOSYS.
    int64_t r = cinux::syscall::do_mknod_kernel("/dev/nope", 0x2000 | 0666);
    TEST_ASSERT_EQ(r, -cinux::kEnosys);
}

// ============================================================
// 3. Duplicate mkfifo is rejected (EEXIST)
// ============================================================

void test_fifo_mknod_duplicate_eexist() {
    cinux::ipc::FifoRegistry::instance().remove("dup");
    TEST_ASSERT_EQ(cinux::syscall::do_mknod_kernel("/dev/dup", cinux::ipc::kSIfFifo | 0666), 0);
    int64_t r = cinux::syscall::do_mknod_kernel("/dev/dup", cinux::ipc::kSIfFifo | 0666);
    TEST_ASSERT_EQ(r, -cinux::kEexist);
    cinux::ipc::FifoRegistry::instance().remove("dup");
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_fifo_tests() {
    TEST_SECTION("FIFO Tests (F8-M2)");

    RUN_TEST(test_fifo_mkfifo_open_roundtrip);
    RUN_TEST(test_fifo_mknod_non_fifo_rejected);
    RUN_TEST(test_fifo_mknod_duplicate_eexist);

    TEST_SUMMARY();
}
