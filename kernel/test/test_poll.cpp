/**
 * @file kernel/test/test_poll.cpp
 * @brief F8-M5 real poll/select mechanism tests (028e)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises:
 *   - do_poll_core readiness on a pipe (data -> POLLIN; empty -> 0; writer
 *     closed -> POLLHUP; bad/closed fd -> POLLNVAL; negative fd ignored);
 *   - a finite-timeout poll that PARKS for real on the read wait queue + the
 *     timer-wake, returning 0 once the timer fires (no yield spin);
 *   - the event-wake path: a poller parked on a pipe's read wait queue is
 *     unblocked when a peer writes.  The harness is single-threaded, so the
 *     infinite-timeout loop itself is role-played under a NoRescheduleGuard
 *     (prepare_to_wait + poll_events register, then a write -> wake_one ->
 *     unblock, observed directly); the full blocking loop runs for real in the
 *     busybox nc/wget/sh smoke.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/poll_core.hpp"
#include "kernel/syscall/sys_pipe.hpp"

using cinux::fs::current_fd_table;
using cinux::fs::Inode;
using cinux::fs::kPollErr;
using cinux::fs::kPollHup;
using cinux::fs::kPollIn;
using cinux::fs::kPollNval;
using cinux::fs::kPollOut;
using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::syscall::do_pipe_kernel;
using cinux::syscall::do_poll_core;
using cinux::syscall::kpollfd;

namespace test_poll {

namespace {

void dummy_entry() {}

class CurrentFdTableGuard {
public:
    CurrentFdTableGuard() : prev_(Scheduler::current()), task_(new Task()) {
        task_->fd_table = new cinux::fs::FDTable();
        Scheduler::set_current(task_);
    }

    ~CurrentFdTableGuard() {
        Scheduler::set_current(prev_);
        delete task_;
    }

private:
    Task* prev_;
    Task* task_;
};

void reserve_stdio_slots() {
    for (int fd = 0; fd <= 2; ++fd) {
        current_fd_table().set(fd, new cinux::fs::File(nullptr, 0, cinux::fs::OpenFlags::RDWR));
    }
}

/// Build a pipe into the current (global) fd table; return its two fds.
void make_pipe(int& rfd, int& wfd) {
    int     kfds[2] = {-1, -1};
    int64_t rc      = do_pipe_kernel(current_fd_table(), kfds);
    TEST_ASSERT_EQ(rc, 0);
    rfd = kfds[0];
    wfd = kfds[1];
}

/// Free both pipe fd slots so later tests inherit a clean table.
void close_fds(int rfd, int wfd) {
    current_fd_table().close(rfd);
    current_fd_table().close(wfd);
}

/// Push bytes through the write-end InodeOps (kernel buffer; no user memory).
int64_t push_write(int wfd, const char* s, size_t n) {
    Inode* wi = current_fd_table().get(wfd)->inode;
    auto   r  = wi->ops->write(wi, 0, s, n);
    return r.ok() ? *r : -1;
}

Inode* read_inode(int rfd) {
    return current_fd_table().get(rfd)->inode;
}

/// One-element poll over @p fd for @p events, timeout 0 (immediate readiness).
int64_t poll_one_immediate(int fd, uint16_t events, uint16_t& revents) {
    kpollfd p;
    p.fd      = fd;
    p.events  = static_cast<int16_t>(events);
    p.revents = 0;
    int64_t r = do_poll_core(&p, 1, 0);
    revents   = static_cast<uint16_t>(p.revents);
    return r;
}

/// One-element poll with an explicit timeout (ms).  A positive timeout PARKS the
/// caller (registered on the fd's wait queue + the timer-wake); used to prove the
/// finite-timeout path returns 0 once the timer fires.
int64_t poll_one_timeout(int fd, uint16_t events, int64_t timeout_ms, uint16_t& revents) {
    kpollfd p;
    p.fd      = fd;
    p.events  = static_cast<int16_t>(events);
    p.revents = 0;
    int64_t r = do_poll_core(&p, 1, timeout_ms);
    revents   = static_cast<uint16_t>(p.revents);
    return r;
}

// ============================================================
// Readiness (immediate, timeout 0)
// ============================================================

/// A pipe with buffered data reports POLLIN and returns 1.
void test_poll_data_ready_pollin() {
    int rfd, wfd;
    make_pipe(rfd, wfd);
    TEST_ASSERT_EQ(push_write(wfd, "hi", 2), 2);

    uint16_t rv = 0;
    TEST_ASSERT_EQ(poll_one_immediate(rfd, kPollIn, rv), 1);
    TEST_ASSERT_EQ(rv, kPollIn);

    close_fds(rfd, wfd);
}

/// An empty pipe (writer still open) reports nothing ready; returns 0.
void test_poll_empty_returns_zero() {
    int rfd, wfd;
    make_pipe(rfd, wfd);

    uint16_t rv = 0xFFFF;
    TEST_ASSERT_EQ(poll_one_immediate(rfd, kPollIn, rv), 0);
    TEST_ASSERT_EQ(rv, 0);

    close_fds(rfd, wfd);
}

/// The write end of a non-full pipe reports POLLOUT.
void test_poll_write_end_pollobut() {
    int rfd, wfd;
    make_pipe(rfd, wfd);

    uint16_t rv = 0;
    TEST_ASSERT_EQ(poll_one_immediate(wfd, kPollOut, rv), 1);
    TEST_ASSERT_EQ(rv, kPollOut);

    close_fds(rfd, wfd);
}

/// With two fds polled together, only the ready one gets revents.
void test_poll_two_fds_one_ready() {
    int rfd1, wfd1;
    make_pipe(rfd1, wfd1);
    int rfd2, wfd2;
    make_pipe(rfd2, wfd2);
    TEST_ASSERT_EQ(push_write(wfd2, "x", 1), 1);  // pipe 2 has data, pipe 1 empty

    kpollfd p[2];
    p[0].fd      = rfd1;
    p[0].events  = static_cast<int16_t>(kPollIn);
    p[0].revents = 0;
    p[1].fd      = rfd2;
    p[1].events  = static_cast<int16_t>(kPollIn);
    p[1].revents = 0;

    TEST_ASSERT_EQ(do_poll_core(p, 2, 0), 1);
    TEST_ASSERT_EQ(p[0].revents, 0);  // empty pipe: not ready
    TEST_ASSERT_EQ(p[1].revents, kPollIn);

    close_fds(rfd1, wfd1);
    close_fds(rfd2, wfd2);
}

/// A finite-timeout poll on an empty pipe PARKS (registered on the read wait
/// queue + the timer-wake) and returns 0 once the timer fires -- the real
/// blocking path, not a yield spin.  Proves the timer-wake end-to-end.
void test_poll_finite_timeout_parks_then_returns_zero() {
    int rfd, wfd;
    make_pipe(rfd, wfd);

    uint16_t rv = 0xFFFF;
    int64_t  r  = poll_one_timeout(rfd, kPollIn, 30, rv);
    TEST_ASSERT_EQ(r, 0);  // nothing became ready -> timeout
    TEST_ASSERT_EQ(rv, 0);

    close_fds(rfd, wfd);
}

/// A negative fd is ignored (revents left 0, does not count).
void test_poll_negative_fd_ignored() {
    kpollfd p;
    p.fd      = -1;
    p.events  = static_cast<int16_t>(kPollIn);
    p.revents = 0x55;  // nonzero sentinel: must be reset to 0 (fd ignored)
    TEST_ASSERT_EQ(do_poll_core(&p, 1, 0), 0);
    TEST_ASSERT_EQ(p.revents, 0);
}

/// A closed fd (no fd-table entry, fd > 2) reports POLLNVAL.
void test_poll_closed_fd_pollnval() {
    CurrentFdTableGuard guard;
    reserve_stdio_slots();

    int rfd, wfd;
    make_pipe(rfd, wfd);
    TEST_ASSERT_GE(rfd, 3);
    current_fd_table().close(rfd);  // rfd is now unused

    uint16_t rv = 0;
    // rfd > 2, so an absent entry is a genuine invalid fd, not legacy stdio.
    TEST_ASSERT_EQ(poll_one_immediate(rfd, kPollIn, rv), 1);
    TEST_ASSERT_EQ(rv, kPollNval);

    current_fd_table().close(wfd);
}

/// Closing the writer makes the read end report POLLHUP (EOF).  Built on a bare
/// Pipe + PipeReadOps because fd close does not yet propagate to Pipe (no
/// InodeOps release hook -- a known hobby-OS limitation); the readiness LOGIC is
/// what matters here, and it lives in Pipe::poll_read_events.
void test_poll_writer_closed_pollhup() {
    // Heap-allocate the Pipe (its 4 KB ring buffer would blow the kernel stack).
    cinux::ipc::Pipe*       pipe = new cinux::ipc::Pipe();
    cinux::ipc::PipeReadOps read_ops(pipe);
    pipe->close_writer();  // reader observes EOF (buffer empty, writer gone)

    bool     registered = true;
    uint32_t mask       = read_ops.poll_events(nullptr, nullptr, &registered);
    TEST_ASSERT_TRUE((mask & kPollHup) != 0);  // EOF -> hangup
    TEST_ASSERT_FALSE(registered);             // waiter=null -> not parked

    delete pipe;
}

// ============================================================
// Wait registration + wake (role-played under NoRescheduleGuard)
// ============================================================

/// A poller parked on a pipe's read wait queue is woken when a peer writes.
/// This is the exact mechanism do_poll_core's infinite-timeout path uses; the
/// harness cannot run the real blocking loop (it would hang), so we drive the
/// commit sequence by hand and observe the wake.
void test_poll_write_wakes_registered_poller() {
    Scheduler::init();
    int rfd, wfd;
    make_pipe(rfd, wfd);
    Inode* ri = read_inode(rfd);

    Task* poller = TaskBuilder().set_entry(dummy_entry).set_name("poll_wake").build();
    TEST_ASSERT_NOT_NULL(poller);
    Scheduler::add_task(poller);
    Scheduler::set_current(poller);

    {
        Scheduler::NoRescheduleGuard guard;

        // Commit to sleeping on the read end (mirror do_poll_core's infinite path):
        // mark Blocked, then register on the pipe's read wait queue.
        Scheduler::prepare_to_wait(poller);  // state -> Blocked
        bool     registered = false;
        uint32_t mask       = ri->ops->poll_events(ri, poller, &registered);
        TEST_ASSERT_TRUE(registered);  // a pipe is a blocking fd -> parks the poller
        TEST_ASSERT_EQ(mask, 0u);      // empty pipe -> nothing ready yet

        // A peer writes one byte: Pipe::write pushes data and wake_one()s the
        // read wait queue, which must unblock our parked poller.
        Inode* wi = current_fd_table().get(wfd)->inode;
        auto   wr = wi->ops->write(wi, 0, "x", 1);
        TEST_ASSERT_TRUE(wr.ok());
        TEST_ASSERT_EQ(static_cast<int>(poller->state), static_cast<int>(TaskState::Ready));

        // After the wake the read end reports POLLIN.
        uint32_t after = ri->ops->poll_events(ri, nullptr, nullptr);
        TEST_ASSERT_TRUE((after & kPollIn) != 0);

        ri->ops->poll_detach_waiter(ri, poller);  // tidy: unlink the waiter
    }

    Scheduler::set_current(nullptr);
    Scheduler::remove_task(poller);
    close_fds(rfd, wfd);
}

}  // namespace

extern "C" void run_poll_tests() {
    TEST_SECTION("Poll / Select Tests (028e)");

    RUN_TEST(test_poll_data_ready_pollin);
    RUN_TEST(test_poll_empty_returns_zero);
    RUN_TEST(test_poll_write_end_pollobut);
    RUN_TEST(test_poll_writer_closed_pollhup);
    RUN_TEST(test_poll_two_fds_one_ready);
    RUN_TEST(test_poll_finite_timeout_parks_then_returns_zero);
    RUN_TEST(test_poll_negative_fd_ignored);
    RUN_TEST(test_poll_closed_fd_pollnval);
    RUN_TEST(test_poll_write_wakes_registered_poller);

    TEST_SUMMARY();
}

}  // namespace test_poll
