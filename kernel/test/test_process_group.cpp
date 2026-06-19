/**
 * @file kernel/test/test_process_group.cpp
 * @brief QEMU in-kernel tests for process-group / session (F3-M3)
 *
 * Batch 1: inherit_process_identity() -- the rule fork()/clone() use to derive
 * a child's pgid/sid/session_leader/controlling_tty.  Root fork (parent
 * pgid==0) founds a new group+session; otherwise the parent's membership is
 * inherited.
 *
 * Batch 2: setpgid/getpgid/getsid/setsid identity operations, plus killpg
 * (signal broadcast over the pid registry).  setpgid/setsid are pure field
 * transforms; killpg registers throwaway tasks on the global registry and
 * unregisters them BEFORE asserting (TEST_ASSERT early-returns, so cleanup
 * first avoids leaking test tasks into later tests -- GOTCHA#19 family).
 *
 * The 4 syscalls (setpgid/getpgid/getsid/setsid) land in batch 3.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_group.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_pgrp.hpp"

using cinux::proc::Task;
using cinux::proc::TaskState;
using cinux::proc::inherit_process_identity;
using cinux::proc::setpgid;
using cinux::proc::getpgid;
using cinux::proc::getsid;
using cinux::proc::setsid;
using cinux::proc::killpg;
using cinux::proc::Signal;
using cinux::proc::SharedSigActions;
using cinux::proc::sig_is_member;
using cinux::proc::signal_register_task;
using cinux::proc::signal_unregister_task;
using cinux::proc::Scheduler;
using cinux::syscall::sys_setpgid;
using cinux::syscall::sys_getpgid;
using cinux::syscall::sys_getsid;
using cinux::syscall::sys_setsid;

// ============================================================
// Path 1: root fork -- child founds its own group and session
// ============================================================

namespace test_pgrp_root_fork {

void test_root_fork_founds_new_group_and_session() {
    // The parent is a kernel/bootstrap task with no group (pgid == 0).
    Task parent{};
    parent.pgid            = 0;
    parent.sid             = 0;
    parent.session_leader  = nullptr;
    parent.controlling_tty = -1;

    Task child{};
    inherit_process_identity(&child, &parent, /*child_pid=*/1);

    // The child becomes its own group leader and session leader.
    TEST_ASSERT_EQ(child.pgid, 1);
    TEST_ASSERT_EQ(child.sid, 1);
    TEST_ASSERT_EQ(child.session_leader, &child);
    TEST_ASSERT_EQ(child.controlling_tty, -1);  // none to inherit
}

void test_root_fork_ignores_stale_parent_session() {
    // Even if the root parent had garbage sid (it should not, but the rule
    // keys off pgid == 0), the child still founds a fresh session.
    Task parent{};
    parent.pgid = 0;
    parent.sid  = 999;  // ignored on the root path

    Task child{};
    inherit_process_identity(&child, &parent, 5);

    TEST_ASSERT_EQ(child.pgid, 5);
    TEST_ASSERT_EQ(child.sid, 5);
    TEST_ASSERT_EQ(child.session_leader, &child);
}

}  // namespace test_pgrp_root_fork

// ============================================================
// Path 2: normal fork -- child inherits the parent's membership
// ============================================================

namespace test_pgrp_inherit {

void test_child_inherits_parent_group_and_session() {
    // A normal process that already belongs to group 1 / session 1 (led by
    // `leader`), and holds controlling terminal index 3.
    Task leader{};
    leader.pgid            = 1;
    leader.sid             = 1;
    leader.session_leader  = &leader;
    leader.controlling_tty = 3;

    Task child{};
    inherit_process_identity(&child, &leader, /*child_pid=*/2);

    TEST_ASSERT_EQ(child.pgid, 1);                  // same group as parent
    TEST_ASSERT_EQ(child.sid, 1);                   // same session
    TEST_ASSERT_EQ(child.session_leader, &leader);  // leader pointer shared
    TEST_ASSERT_EQ(child.controlling_tty, 3);       // tty inherited
}

void test_inherit_preserves_arbitrary_pgid_sid() {
    // setpgid() (batch 2) will move a process into an arbitrary group; a fork
    // from it must carry that group forward verbatim.
    Task parent{};
    parent.pgid            = 42;
    parent.sid             = 17;
    parent.controlling_tty = 7;

    Task child{};
    inherit_process_identity(&child, &parent, 99);

    TEST_ASSERT_EQ(child.pgid, 42);
    TEST_ASSERT_EQ(child.sid, 17);
    TEST_ASSERT_EQ(child.controlling_tty, 7);
}

}  // namespace test_pgrp_inherit

// ============================================================
// End-to-end chain: kernel_init -> init -> grandchild
// ============================================================

namespace test_pgrp_chain {

void test_kernel_init_to_init_to_grandchild() {
    // kernel_init_thread (pid 0, no group) forks the first user process.
    Task kinit{};
    kinit.pgid = 0;

    Task init_t{};
    inherit_process_identity(&init_t, &kinit, /*child_pid=*/1);
    TEST_ASSERT_EQ(init_t.pgid, 1);  // init founds group 1
    TEST_ASSERT_EQ(init_t.sid, 1);
    TEST_ASSERT_EQ(init_t.session_leader, &init_t);

    // init forks a child, which must stay in init's group/session.
    Task grandchild{};
    inherit_process_identity(&grandchild, &init_t, /*child_pid=*/2);
    TEST_ASSERT_EQ(grandchild.pgid, 1);  // inherited, not refounded
    TEST_ASSERT_EQ(grandchild.sid, 1);
    TEST_ASSERT_EQ(grandchild.session_leader, &init_t);
}

}  // namespace test_pgrp_chain

// ============================================================
// Batch 2: setpgid / getpgid / getsid identity operations
// ============================================================

namespace test_pgrp_setpgid {

void test_setpgid_zero_means_own_pid() {
    Task t{};
    t.pid = 7;
    TEST_ASSERT_EQ(setpgid(&t, 0), 0);
    TEST_ASSERT_EQ(t.pgid, 7);  // leads a new group whose id is its own pid
}

void test_setpgid_explicit_group() {
    Task t{};
    t.pid = 7;
    TEST_ASSERT_EQ(setpgid(&t, 5), 0);
    TEST_ASSERT_EQ(t.pgid, 5);
}

void test_setpgid_rejects_null_and_negative() {
    TEST_ASSERT_EQ(setpgid(nullptr, 0), -3);  // ESRCH
    Task t{};
    t.pid = 1;
    TEST_ASSERT_EQ(setpgid(&t, -1), -22);  // EINVAL
}

void test_getpgid_getsid_read_back() {
    Task t{};
    t.pid  = 8;
    t.pgid = 4;
    t.sid  = 6;
    TEST_ASSERT_EQ(getpgid(&t), 4);
    TEST_ASSERT_EQ(getsid(&t), 6);
    TEST_ASSERT_EQ(getpgid(nullptr), -3);  // ESRCH
}

}  // namespace test_pgrp_setpgid

namespace test_pgrp_setsid {

void test_setsid_creates_new_session() {
    Task t{};
    t.pid             = 9;
    t.pgid            = 1;  // not yet a leader (pgid != pid)
    t.sid             = 1;
    t.controlling_tty = 3;

    TEST_ASSERT_EQ(setsid(&t), 9);  // returns the new sid
    TEST_ASSERT_EQ(t.pgid, 9);
    TEST_ASSERT_EQ(t.sid, 9);
    TEST_ASSERT_EQ(t.session_leader, &t);
    TEST_ASSERT_EQ(t.controlling_tty, -1);  // tty cleared on new session
}

void test_setsid_eperm_if_already_leader() {
    Task t{};
    t.pid  = 5;
    t.pgid = 5;                      // already leads a process group
    TEST_ASSERT_EQ(setsid(&t), -1);  // EPERM
    TEST_ASSERT_EQ(t.pgid, 5);       // unchanged
}

void test_setsid_esrch_on_null() {
    TEST_ASSERT_EQ(setsid(nullptr), -3);
}

}  // namespace test_pgrp_setsid

// ============================================================
// Batch 2: killpg -- signal broadcast over the pid registry
// ============================================================

namespace test_killpg {

// Tasks live on the test's stack and are explicitly unregistered BEFORE any
// assert: TEST_ASSERT early-returns on failure, so cleanup-first avoids
// leaking test tasks into the global registry for later tests.

void test_killpg_broadcasts_only_to_group_members() {
    Task a{}, b{}, c{};
    a.pid         = 10;
    a.pgid        = 1;
    a.sig_actions = SharedSigActions::create();
    b.pid         = 11;
    b.pgid        = 1;
    b.sig_actions = SharedSigActions::create();
    c.pid         = 12;
    c.pgid        = 2;
    c.sig_actions = SharedSigActions::create();
    a.state = b.state = c.state = TaskState::Ready;

    signal_register_task(&a);
    signal_register_task(&b);
    signal_register_task(&c);

    int  sent  = killpg(1, Signal::kSigusr1);
    bool a_got = sig_is_member(a.sig_pending, Signal::kSigusr1);
    bool b_got = sig_is_member(b.sig_pending, Signal::kSigusr1);
    bool c_got = sig_is_member(c.sig_pending, Signal::kSigusr1);

    signal_unregister_task(&a);
    signal_unregister_task(&b);
    signal_unregister_task(&c);
    a.sig_actions->release();
    b.sig_actions->release();
    c.sig_actions->release();

    TEST_ASSERT_EQ(sent, 2);  // only a and b are in group 1
    TEST_ASSERT(a_got);
    TEST_ASSERT(b_got);
    TEST_ASSERT(!c_got);  // c is in group 2, untouched
}

void test_killpg_empty_group_returns_zero() {
    Task a{};
    a.pid         = 20;
    a.pgid        = 1;
    a.sig_actions = SharedSigActions::create();
    a.state       = TaskState::Ready;
    signal_register_task(&a);

    int sent = killpg(999, Signal::kSigusr2);  // no such group

    signal_unregister_task(&a);
    a.sig_actions->release();

    TEST_ASSERT_EQ(sent, 0);
}

}  // namespace test_killpg

// ============================================================
// Batch 3: process-group / session syscalls (via the handlers, pid 0 => self)
// ============================================================

namespace test_pgrp_syscall {

// The scheduler loop does not run during the suite, so each test installs its
// task as current via Scheduler::set_current() -- which sets BOTH the static
// current_ (what Scheduler::current() actually reads) AND g_per_cpu.current.
// Setting only g_per_cpu.current leaves current_ null and makes the handlers
// see no caller (ESRCH).  Cleared BEFORE asserting (TEST_ASSERT early-returns,
// GOTCHA#19 family).

void test_sys_setpgid_and_getpgid() {
    Task t{};
    t.pid          = 30;
    t.pgid         = 1;  // not yet a leader
    t.tgid         = 30;
    t.group_leader = &t;
    t.sig_actions  = SharedSigActions::create();
    Scheduler::set_current(&t);

    int64_t r_set  = sys_setpgid(0, 5, 0, 0, 0, 0);  // pid 0 => self
    int64_t r_get  = sys_getpgid(0, 0, 0, 0, 0, 0);
    int64_t p_pgid = t.pgid;

    Scheduler::set_current(nullptr);  // cleanup BEFORE asserts
    t.sig_actions->release();

    TEST_ASSERT_EQ(r_set, 0);
    TEST_ASSERT_EQ(p_pgid, 5);
    TEST_ASSERT_EQ(r_get, 5);
}

void test_sys_setsid_and_getsid() {
    Task t{};
    t.pid          = 31;
    t.pgid         = 1;  // not yet a leader
    t.tgid         = 31;
    t.group_leader = &t;
    t.sig_actions  = SharedSigActions::create();
    Scheduler::set_current(&t);

    int64_t r_sid  = sys_setsid(0, 0, 0, 0, 0, 0);
    int64_t r_get  = sys_getsid(0, 0, 0, 0, 0, 0);
    int64_t p_pgid = t.pgid;
    int64_t p_sid  = t.sid;

    Scheduler::set_current(nullptr);
    t.sig_actions->release();

    TEST_ASSERT_EQ(r_sid, 31);  // new sid == pid
    TEST_ASSERT_EQ(p_pgid, 31);
    TEST_ASSERT_EQ(p_sid, 31);
    TEST_ASSERT_EQ(r_get, 31);
}

void test_sys_getpgid_esrch_for_unknown_pid() {
    Task t{};
    t.pid          = 30;
    t.tgid         = 30;
    t.group_leader = &t;
    t.sig_actions  = SharedSigActions::create();
    Scheduler::set_current(&t);

    // pid 999 is neither the caller (30) nor registered => ESRCH.
    int64_t r = sys_getpgid(999, 0, 0, 0, 0, 0);

    Scheduler::set_current(nullptr);
    t.sig_actions->release();

    TEST_ASSERT_EQ(r, -3);  // ESRCH
}

}  // namespace test_pgrp_syscall

extern "C" void run_process_group_tests() {
    TEST_SECTION("Process Group Tests (F3-M3-1)");

    RUN_TEST(test_pgrp_root_fork::test_root_fork_founds_new_group_and_session);
    RUN_TEST(test_pgrp_root_fork::test_root_fork_ignores_stale_parent_session);

    RUN_TEST(test_pgrp_inherit::test_child_inherits_parent_group_and_session);
    RUN_TEST(test_pgrp_inherit::test_inherit_preserves_arbitrary_pgid_sid);

    RUN_TEST(test_pgrp_chain::test_kernel_init_to_init_to_grandchild);

    RUN_TEST(test_pgrp_setpgid::test_setpgid_zero_means_own_pid);
    RUN_TEST(test_pgrp_setpgid::test_setpgid_explicit_group);
    RUN_TEST(test_pgrp_setpgid::test_setpgid_rejects_null_and_negative);
    RUN_TEST(test_pgrp_setpgid::test_getpgid_getsid_read_back);

    RUN_TEST(test_pgrp_setsid::test_setsid_creates_new_session);
    RUN_TEST(test_pgrp_setsid::test_setsid_eperm_if_already_leader);
    RUN_TEST(test_pgrp_setsid::test_setsid_esrch_on_null);

    RUN_TEST(test_killpg::test_killpg_broadcasts_only_to_group_members);
    RUN_TEST(test_killpg::test_killpg_empty_group_returns_zero);

    RUN_TEST(test_pgrp_syscall::test_sys_setpgid_and_getpgid);
    RUN_TEST(test_pgrp_syscall::test_sys_setsid_and_getsid);
    RUN_TEST(test_pgrp_syscall::test_sys_getpgid_esrch_for_unknown_pid);

    TEST_SUMMARY();
}
