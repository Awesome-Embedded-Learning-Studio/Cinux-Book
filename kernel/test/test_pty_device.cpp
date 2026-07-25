/**
 * @file kernel/test/test_pty_device.cpp
 * @brief In-kernel tests for the PTY device layer (F10-M3 Phase 2 batch 3)
 *
 * Runs inside QEMU via run-kernel-test.  Exercises the PTY registry + master/slave
 * InodeOps directly: alloc, master<->slave round-trip, echo, the slave->Pty
 * wiring, and ioctl dispatch.  The read/write paths take kernel buffers (no user
 * boundary), so they are fully testable in ring0; the termios-copy ioctls
 * (TCGETS/TCSETS/TIOCGPTN) cross the user boundary via copy_to/from_user, which
 * the ring0 harness cannot exercise (access_ok rejects kernel addresses) -- those
 * are covered by the batch-1 host tests (pure Pty termios) and the batch-5
 * end-to-end smoke (real sys_ioctl from a user program).
 */

#include <stdint.h>

#include "kernel/drivers/tty/pty.hpp"         // Pty (white-box slave wiring check)
#include "kernel/drivers/tty/pty_device.hpp"  // pty_alloc/release/master/slave, kTiocgptn
#include "kernel/drivers/tty/tty.hpp"         // kIcanon, kTiocsctty
#include "kernel/fs/inode.hpp"
#include "kernel/proc/process.hpp"    // Task (TIOCSCTTY controlling-tty test)
#include "kernel/proc/scheduler.hpp"  // Scheduler::set_current/current
#include "kernel/test/big_kernel_test.h"

using namespace cinux::drivers;
using cinux::fs::Inode;
using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::drivers::Pty;
using cinux::drivers::kIcanon;
using cinux::drivers::kTiocsctty;

namespace test_pty_device {

void test_alloc_and_slave_lookup() {
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    TEST_ASSERT_TRUE(*idx >= 0);
    Inode* master = pty_master_inode(*idx);
    Inode* slave  = pty_slave_inode(*idx).value();
    TEST_ASSERT_NOT_NULL(master);
    TEST_ASSERT_NOT_NULL(slave);
    TEST_ASSERT_FALSE(pty_slave_inode(999).ok());  // bad index -> NotFound
    pty_release(*idx);
}

void test_master_write_slave_read_cooked() {
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* master = pty_master_inode(*idx);
    Inode* slave  = pty_slave_inode(*idx).value();
    TEST_ASSERT_EQ(write_or_neg1(master, 0, "hi\n", 3), 3);
    char    buf[16];
    int64_t r = read_or_neg1(slave, 0, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 3);
    TEST_ASSERT_TRUE(buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');
    pty_release(*idx);
}

void test_slave_write_master_read_output() {
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* master = pty_master_inode(*idx);
    Inode* slave  = pty_slave_inode(*idx).value();
    TEST_ASSERT_EQ(write_or_neg1(slave, 0, "out", 3), 3);
    char    buf[16];
    int64_t r = read_or_neg1(master, 0, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 3);
    TEST_ASSERT_TRUE(buf[0] == 'o' && buf[1] == 'u' && buf[2] == 't');
    pty_release(*idx);
}

void test_local_echo_routes_to_master() {
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* master = pty_master_inode(*idx);
    TEST_ASSERT_EQ(write_or_neg1(master, 0, "ab\n", 3), 3);
    char    buf[16];
    int64_t r = read_or_neg1(master, 0, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 3);  // echo of the typed line
    TEST_ASSERT_TRUE(buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\n');
    pty_release(*idx);
}

void test_slave_wired_to_cooked_termios() {
    // White-box: the slave inode's fs_private is the pair's Pty, whose line
    // discipline starts in cooked mode (ICANON|ECHO|ISIG).  Confirms the slave
    // inode -> Pty wiring without crossing the user boundary.
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* slave = pty_slave_inode(*idx).value();
    auto*  p     = static_cast<Pty*>(slave->fs_private);
    TEST_ASSERT_NOT_NULL(p);
    // TEST_ASSERT_TRUE compares == true, so coerce the bitmask to a real bool.
    TEST_ASSERT_TRUE((p->slave_tty().termios().c_lflag & kIcanon) != 0);
    pty_release(*idx);
}

void test_unknown_slave_ioctl_refused() {
    // The slave InodeOps::ioctl dispatches (the batch-2 seam at the device
    // layer) and refuses an unknown request with NotImplemented (-> -ENOTTY in
    // sys_ioctl).  Asserts the dispatch reaches the device, not the value of
    // the copy path.
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* slave = pty_slave_inode(*idx).value();
    auto   r     = slave->ops->ioctl(slave, 0x12345678, 0);
    TEST_ASSERT_FALSE(r.ok());
    pty_release(*idx);
}

void test_release_lets_slot_reuse() {
    auto a = pty_alloc();
    TEST_ASSERT_TRUE(a.ok());
    pty_release(*a);
    auto b = pty_alloc();  // should reuse the freed slot
    TEST_ASSERT_TRUE(b.ok());
    TEST_ASSERT_EQ(*b, *a);  // same index reclaimed
    pty_release(*b);
}

// ---- TIOCSCTTY / controlling terminal (batch 4) ----
//
// These install a stack Task as current via Scheduler::set_current (mirrors
// test_clone / test_brk).  arg to TIOCSCTTY is the steal flag, carried inline in
// the ioctl word (no user pointer), so the path is testable in ring0.

void test_tiocsctty_refused_for_non_leader() {
    // A task that is not a session leader (session_leader == nullptr) is refused.
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* slave = pty_slave_inode(*idx).value();
    Task   t{};  // session_leader defaults to nullptr -> not a leader
    auto*  prev = Scheduler::current();
    Scheduler::set_current(&t);
    auto r = slave->ops->ioctl(slave, kTiocsctty, 0);
    Scheduler::set_current(prev);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(t.controlling_tty, -1);  // unchanged
    pty_release(*idx);
}

void test_tiocsctty_acquired_when_session_leader() {
    auto idx = pty_alloc();
    TEST_ASSERT_TRUE(idx.ok());
    Inode* slave = pty_slave_inode(*idx).value();
    Task   t{};
    t.session_leader = &t;  // a session leader may acquire
    auto* prev       = Scheduler::current();
    Scheduler::set_current(&t);
    auto r = slave->ops->ioctl(slave, kTiocsctty, 0);
    Scheduler::set_current(prev);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQ(t.controlling_tty, *idx);  // acquired the pty index
    pty_release(*idx);
}

}  // namespace test_pty_device

extern "C" void run_pty_device_tests() {
    TEST_SECTION("PTY device (F10-M3 Phase 2)");
    RUN_TEST(test_pty_device::test_alloc_and_slave_lookup);
    RUN_TEST(test_pty_device::test_master_write_slave_read_cooked);
    RUN_TEST(test_pty_device::test_slave_write_master_read_output);
    RUN_TEST(test_pty_device::test_local_echo_routes_to_master);
    RUN_TEST(test_pty_device::test_slave_wired_to_cooked_termios);
    RUN_TEST(test_pty_device::test_unknown_slave_ioctl_refused);
    RUN_TEST(test_pty_device::test_release_lets_slot_reuse);
    RUN_TEST(test_pty_device::test_tiocsctty_refused_for_non_leader);
    RUN_TEST(test_pty_device::test_tiocsctty_acquired_when_session_leader);
    TEST_SUMMARY();
}
