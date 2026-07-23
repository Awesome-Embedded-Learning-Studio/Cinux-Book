/**
 * @file kernel/test/test_sys_pipe.cpp
 * @brief QEMU in-kernel tests for sys_pipe integration (031 Phase 2)
 *
 * Test coverage:
 *   - FDTable::set() installs File at specific slots (0, 1)
 *   - Pipe + InodeOps write/read round-trip through FDTable
 *   - Close read end then write returns -1 (through FDTable)
 *   - Close write end then read returns 0 (EOF, through FDTable)
 *   - Drain remaining data then EOF
 *   - Multiple write/read cycles
 *   - FDTable::set() out-of-range rejection
 *   - sys_pipe rejects null/invalid user addresses
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (for Pipe/Inode/InodeOps allocation)
 */

#include "big_kernel_test.h"
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"  // g_global_fd_table (do_write_kernel fd resolution)
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/proc/process.hpp"  // Task (SIGPIPE test current-task scratch)
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // sig_is_member / Signal::kSigpipe
#include "kernel/syscall/sys_pipe.hpp"
#include "kernel/syscall/sys_write.hpp"  // do_write_kernel (SIGPIPE end-to-end)

using cinux::fs::FDTable;
using cinux::fs::File;
using cinux::fs::Inode;
using cinux::fs::InodeType;
using cinux::fs::OpenFlags;
using cinux::ipc::Pipe;
using cinux::ipc::PipeReadOps;
using cinux::ipc::PipeWriteOps;

// ============================================================
// 1. FDTable::set() at slot 0 and 1
// ============================================================

void test_sys_pipe_fdtable_set_slot0() {
    FDTable table;
    Inode   inode{};

    File* f  = new File(&inode, 0, OpenFlags::RDONLY);
    bool  ok = table.set(0, f);
    TEST_ASSERT_TRUE(ok);

    File* retrieved = table.get(0);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_TRUE(retrieved == f);

    delete retrieved;
}

void test_sys_pipe_fdtable_set_slot1() {
    FDTable table;
    Inode   inode{};

    File* f  = new File(&inode, 0, OpenFlags::WRONLY);
    bool  ok = table.set(1, f);
    TEST_ASSERT_TRUE(ok);

    File* retrieved = table.get(1);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_TRUE(retrieved->flags == OpenFlags::WRONLY);

    delete retrieved;
}

// ============================================================
// 2. FDTable::set() out-of-range rejection
// ============================================================

void test_sys_pipe_fdtable_set_negative() {
    FDTable table;
    Inode   inode{};
    File    f(&inode, 0, OpenFlags::RDONLY);

    TEST_ASSERT_FALSE(table.set(-1, &f));
}

void test_sys_pipe_fdtable_set_at_table_size() {
    FDTable table;
    Inode   inode{};
    File    f(&inode, 0, OpenFlags::RDONLY);

    TEST_ASSERT_FALSE(table.set(static_cast<int>(cinux::fs::FD_TABLE_SIZE), &f));
}

// ============================================================
// 3. Pipe + FDTable::set() write/read round-trip
// ============================================================

void test_sys_pipe_write_read_roundtrip() {
    FDTable table;

    // Create pipe with Inodes
    Pipe* pipe      = new Pipe();
    auto* read_ops  = new PipeReadOps(pipe);
    auto* write_ops = new PipeWriteOps(pipe);

    Inode* read_inode = new Inode();
    read_inode->ops   = read_ops;
    read_inode->type  = InodeType::Regular;

    Inode* write_inode = new Inode();
    write_inode->ops   = write_ops;
    write_inode->type  = InodeType::Regular;

    // Install at slots 0 (read) and 1 (write)
    File* read_file  = new File(read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(write_inode, 0, OpenFlags::WRONLY);

    TEST_ASSERT_TRUE(table.set(0, read_file));
    TEST_ASSERT_TRUE(table.set(1, write_file));

    // Write data through write inode's ops
    const char msg[] = "KernelPipe";
    int64_t    w     = write_or_neg1(write_inode, 0, msg, 10);
    TEST_ASSERT_EQ(w, 10);

    // Read data through read inode's ops
    char    buf[16] = {};
    int64_t r       = read_or_neg1(read_inode, 0, buf, 10);
    TEST_ASSERT_EQ(r, 10);

    // Verify content
    TEST_ASSERT_EQ(buf[0], 'K');
    TEST_ASSERT_EQ(buf[1], 'e');
    TEST_ASSERT_EQ(buf[2], 'r');
    TEST_ASSERT_EQ(buf[3], 'n');
    TEST_ASSERT_EQ(buf[4], 'e');
    TEST_ASSERT_EQ(buf[5], 'l');
    TEST_ASSERT_EQ(buf[6], 'P');
    TEST_ASSERT_EQ(buf[7], 'i');
    TEST_ASSERT_EQ(buf[8], 'p');
    TEST_ASSERT_EQ(buf[9], 'e');

    // Cleanup
    delete write_file;
    delete read_file;
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// 4. Close read end, then write returns -1
// ============================================================

void test_sys_pipe_write_after_close_reader() {
    FDTable table;

    Pipe* pipe      = new Pipe();
    auto* read_ops  = new PipeReadOps(pipe);
    auto* write_ops = new PipeWriteOps(pipe);

    Inode* read_inode = new Inode();
    read_inode->ops   = read_ops;

    Inode* write_inode = new Inode();
    write_inode->ops   = write_ops;

    File* read_file  = new File(read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(write_inode, 0, OpenFlags::WRONLY);

    table.set(0, read_file);
    table.set(1, write_file);

    // Close the reader
    pipe->close_reader();

    // Write should fail
    int64_t w = write_or_neg1(write_inode, 0, "data", 4);
    TEST_ASSERT_EQ(w, -1);

    delete write_file;
    delete read_file;
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// 5. Close write end, then read returns 0 (EOF)
// ============================================================

void test_sys_pipe_read_eof_after_close_writer() {
    FDTable table;

    Pipe* pipe      = new Pipe();
    auto* read_ops  = new PipeReadOps(pipe);
    auto* write_ops = new PipeWriteOps(pipe);

    Inode* read_inode = new Inode();
    read_inode->ops   = read_ops;

    Inode* write_inode = new Inode();
    write_inode->ops   = write_ops;

    File* read_file  = new File(read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(write_inode, 0, OpenFlags::WRONLY);

    table.set(0, read_file);
    table.set(1, write_file);

    // Close the writer
    pipe->close_writer();

    // Read should return 0 (EOF)
    char    buf[16] = {};
    int64_t r       = read_or_neg1(read_inode, 0, buf, 8);
    TEST_ASSERT_EQ(r, 0);

    delete write_file;
    delete read_file;
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// 6. Drain then EOF
// ============================================================

void test_sys_pipe_drain_then_eof() {
    FDTable table;

    Pipe* pipe      = new Pipe();
    auto* read_ops  = new PipeReadOps(pipe);
    auto* write_ops = new PipeWriteOps(pipe);

    Inode* read_inode = new Inode();
    read_inode->ops   = read_ops;

    Inode* write_inode = new Inode();
    write_inode->ops   = write_ops;

    File* read_file  = new File(read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(write_inode, 0, OpenFlags::WRONLY);

    table.set(0, read_file);
    table.set(1, write_file);

    // Write data
    int64_t w = write_or_neg1(write_inode, 0, "AB", 2);
    TEST_ASSERT_EQ(w, 2);

    // Close writer
    pipe->close_writer();

    // Drain remaining data
    char    buf[16] = {};
    int64_t r       = read_or_neg1(read_inode, 0, buf, 8);
    TEST_ASSERT_EQ(r, 2);
    TEST_ASSERT_EQ(buf[0], 'A');
    TEST_ASSERT_EQ(buf[1], 'B');

    // Now EOF
    r = read_or_neg1(read_inode, 0, buf, 8);
    TEST_ASSERT_EQ(r, 0);

    delete write_file;
    delete read_file;
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// 7. Multiple write/read cycles
// ============================================================

void test_sys_pipe_multiple_cycles() {
    FDTable table;

    Pipe* pipe      = new Pipe();
    auto* read_ops  = new PipeReadOps(pipe);
    auto* write_ops = new PipeWriteOps(pipe);

    Inode* read_inode = new Inode();
    read_inode->ops   = read_ops;

    Inode* write_inode = new Inode();
    write_inode->ops   = write_ops;

    File* read_file  = new File(read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(write_inode, 0, OpenFlags::WRONLY);

    table.set(0, read_file);
    table.set(1, write_file);

    // First cycle
    TEST_ASSERT_EQ(write_or_neg1(write_inode, 0, "AB", 2), 2);
    char buf[8] = {};
    TEST_ASSERT_EQ(read_or_neg1(read_inode, 0, buf, 2), 2);
    TEST_ASSERT_EQ(buf[0], 'A');
    TEST_ASSERT_EQ(buf[1], 'B');

    // Second cycle
    TEST_ASSERT_EQ(write_or_neg1(write_inode, 0, "CDEF", 4), 4);
    TEST_ASSERT_EQ(read_or_neg1(read_inode, 0, buf, 4), 4);
    TEST_ASSERT_EQ(buf[0], 'C');
    TEST_ASSERT_EQ(buf[1], 'D');
    TEST_ASSERT_EQ(buf[2], 'E');
    TEST_ASSERT_EQ(buf[3], 'F');

    delete write_file;
    delete read_file;
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// 8. FDTable::set() then close releases correctly
// ============================================================

void test_sys_pipe_set_then_close() {
    FDTable table;
    Inode   inode{};
    File*   f = new File(&inode, 0, OpenFlags::RDONLY);

    TEST_ASSERT_TRUE(table.set(0, f));
    TEST_ASSERT_NOT_NULL(table.get(0));

    int ret = table.close(0);
    TEST_ASSERT_EQ(ret, 0);
    TEST_ASSERT_NULL(table.get(0));
}

// ============================================================
// 9. FDTable::set() replaces existing entry
// ============================================================

void test_sys_pipe_set_replaces() {
    FDTable table;
    Inode   inode1{};
    Inode   inode2{};

    File* f1 = new File(&inode1, 0, OpenFlags::RDONLY);
    File* f2 = new File(&inode2, 0, OpenFlags::WRONLY);

    TEST_ASSERT_TRUE(table.set(0, f1));
    TEST_ASSERT_TRUE(table.set(0, f2));

    File* retrieved = table.get(0);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_TRUE(retrieved == f2);
    TEST_ASSERT_TRUE(retrieved->flags == OpenFlags::WRONLY);

    delete f1;
    delete retrieved;
}

// ============================================================
// 10. sys_pipe rejects null user address
// ============================================================

void test_sys_pipe_rejects_null() {
    int64_t r = cinux::syscall::sys_pipe(0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r, -cinux::kEfault);
}

// ============================================================
// 11. sys_pipe rejects kernel-space address
// ============================================================

void test_sys_pipe_rejects_kernel_addr() {
    // 0xFFFFFFFF80100000 is a kernel-space address (bit 47 set)
    int64_t r = cinux::syscall::sys_pipe(0xFFFFFFFF80100000ULL, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r, -cinux::kEfault);
}

// ============================================================
// 12. FDTable::set() preserves File fields
// ============================================================

void test_sys_pipe_set_preserves_fields() {
    FDTable table;
    Inode   inode{};
    inode.ino  = 42;
    inode.size = 1024;

    File* f = new File(&inode, 99, OpenFlags::RDWR);
    TEST_ASSERT_TRUE(table.set(5, f));

    File* retrieved = table.get(5);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_TRUE(retrieved->inode == &inode);
    TEST_ASSERT_EQ(retrieved->offset, 99ULL);
    TEST_ASSERT_TRUE(retrieved->flags == OpenFlags::RDWR);

    delete retrieved;
}

// ============================================================
// 13. SIGPIPE: write to a pipe whose reader closed -> -EPIPE + SIGPIPE (F8-M1)
// ============================================================

namespace {
// RAII: point Scheduler::current() at a scratch Task so do_write_kernel's
// SIGPIPE delivery targets a task whose sig_pending we can inspect. Mirrors the
// CurrentTaskGuard pattern in test_signal.cpp / test_creds.cpp.
struct CurrentTaskGuard {
    cinux::proc::Task* prev;
    explicit CurrentTaskGuard(cinux::proc::Task* task) : prev(cinux::proc::Scheduler::current()) {
        cinux::proc::Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { cinux::proc::Scheduler::set_current(prev); }
};
}  // namespace

// Closing the read end, then writing through do_write_kernel must return -EPIPE
// AND raise SIGPIPE on the writer.  Before F8-M1 this was masked: PipeWriteOps
// returned IOError -> -EIO, so sys_write's kEpipe branch never fired.
void test_sys_pipe_sigpipe_on_broken_write() {
    // Build a pipe; install both ends in the GLOBAL fd table, because
    // do_write_kernel resolves fds through current_fd_table() (== the global
    // table in the harness).
    auto*  pipe        = new Pipe();
    auto*  read_ops    = new PipeReadOps(pipe);
    auto*  write_ops   = new PipeWriteOps(pipe);
    Inode* read_inode  = new Inode();
    Inode* write_inode = new Inode();
    read_inode->ops    = read_ops;
    write_inode->ops   = write_ops;

    int rfd = cinux::fs::g_global_fd_table().alloc(read_inode, OpenFlags::RDONLY);
    int wfd = cinux::fs::g_global_fd_table().alloc(write_inode, OpenFlags::WRONLY);
    TEST_ASSERT_GE(rfd, 0);
    TEST_ASSERT_GE(wfd, 0);

    // Reader gone -> BrokenPipe -> -EPIPE + SIGPIPE queued on current.
    pipe->close_reader();

    // signal_send() dereferences sig_actions, so the scratch Task needs a real
    // SharedSigActions (same wiring as test_signal). {} value-inits sig_pending=0.
    cinux::proc::Task self{};
    self.sig_actions = cinux::proc::SharedSigActions::create();
    {
        CurrentTaskGuard guard(&self);

        int64_t r = cinux::syscall::do_write_kernel(wfd, "x", 1);
        TEST_ASSERT_EQ(r, -cinux::kEpipe);
        // SIGPIPE really was queued (the regression this test guards against:
        // previously the write returned -EIO and no signal was sent).
        TEST_ASSERT_TRUE(
            cinux::proc::sig_is_member(self.sig_pending, cinux::proc::Signal::kSigpipe));
    }

    // Cleanup the global table (close frees the File; pipe/inode/ops are ours).
    cinux::fs::g_global_fd_table().close(rfd);
    cinux::fs::g_global_fd_table().close(wfd);
    delete write_inode;
    delete read_inode;
    delete write_ops;
    delete read_ops;
    delete pipe;
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_sys_pipe_tests() {
    TEST_SECTION("Sys Pipe Tests (031 Phase 2)");

    RUN_TEST(test_sys_pipe_fdtable_set_slot0);
    RUN_TEST(test_sys_pipe_fdtable_set_slot1);
    RUN_TEST(test_sys_pipe_fdtable_set_negative);
    RUN_TEST(test_sys_pipe_fdtable_set_at_table_size);
    RUN_TEST(test_sys_pipe_write_read_roundtrip);
    RUN_TEST(test_sys_pipe_write_after_close_reader);
    RUN_TEST(test_sys_pipe_read_eof_after_close_writer);
    RUN_TEST(test_sys_pipe_drain_then_eof);
    RUN_TEST(test_sys_pipe_multiple_cycles);
    RUN_TEST(test_sys_pipe_set_then_close);
    RUN_TEST(test_sys_pipe_set_replaces);
    RUN_TEST(test_sys_pipe_rejects_null);
    RUN_TEST(test_sys_pipe_rejects_kernel_addr);
    RUN_TEST(test_sys_pipe_set_preserves_fields);
    RUN_TEST(test_sys_pipe_sigpipe_on_broken_write);

    TEST_SUMMARY();
}
