/**
 * @file kernel/test/test_creds.cpp
 * @brief Process-credential tests (F9 batch 9 / M3)
 *
 * Verifies the credential fields default to root (0); the getuid/setuid family
 * round-trips through the syscall handlers (root全能 / non-root EPERM + drop to
 * real); and that a fork-style whole-Task memcpy carries the credentials across
 * (the production fork()/clone() inheritance path, unit-tested in isolation).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/errno.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_creds.hpp"

using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::syscall::sys_getegid;
using cinux::syscall::sys_geteuid;
using cinux::syscall::sys_getgid;
using cinux::syscall::sys_getuid;
using cinux::syscall::sys_setgid;
using cinux::syscall::sys_setuid;

namespace {

/// RAII: install @p task as current, restore the previous on destruction.
struct CurrentTaskGuard {
    Task* prev;
    explicit CurrentTaskGuard(Task* task) : prev(Scheduler::current()) {
        Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { Scheduler::set_current(prev); }
};

}  // namespace

namespace test_creds {

void test_defaults_root() {
    Task t{};
    TEST_ASSERT_EQ(t.uid, 0u);
    TEST_ASSERT_EQ(t.euid, 0u);
    TEST_ASSERT_EQ(t.gid, 0u);
    TEST_ASSERT_EQ(t.egid, 0u);
}

void test_get_returns_current() {
    Task t{};
    t.uid  = 100;
    t.euid = 200;
    t.gid  = 300;
    t.egid = 400;
    CurrentTaskGuard guard(&t);
    TEST_ASSERT_EQ(sys_getuid(0, 0, 0, 0, 0, 0), 100);
    TEST_ASSERT_EQ(sys_geteuid(0, 0, 0, 0, 0, 0), 200);
    TEST_ASSERT_EQ(sys_getgid(0, 0, 0, 0, 0, 0), 300);
    TEST_ASSERT_EQ(sys_getegid(0, 0, 0, 0, 0, 0), 400);
}

void test_setuid_root_can_set_any() {
    Task             t{};  // euid 0 => root
    CurrentTaskGuard guard(&t);
    TEST_ASSERT_EQ(sys_setuid(1000, 0, 0, 0, 0, 0), 0);
    TEST_ASSERT_EQ(t.euid, 1000u);
    // root may return to 0 (uid==0 == real, also allowed for non-root)
    TEST_ASSERT_EQ(sys_setuid(0, 0, 0, 0, 0, 0), 0);
    TEST_ASSERT_EQ(t.euid, 0u);
}

void test_setuid_nonroot_drop_to_real_only() {
    Task t{};
    t.uid  = 100;
    t.euid = 100;  // non-root, real == effective
    CurrentTaskGuard guard(&t);
    // dropping back to real (100) is allowed (a no-op here)
    TEST_ASSERT_EQ(sys_setuid(100, 0, 0, 0, 0, 0), 0);
    TEST_ASSERT_EQ(t.euid, 100u);
    // escalating to any other id is rejected
    TEST_ASSERT_EQ(sys_setuid(200, 0, 0, 0, 0, 0), -cinux::kEperm);
    TEST_ASSERT_EQ(t.euid, 100u);  // unchanged
}

void test_setgid_mirror() {
    Task             t{};  // egid 0 => root
    CurrentTaskGuard guard(&t);
    TEST_ASSERT_EQ(sys_setgid(5, 0, 0, 0, 0, 0), 0);
    TEST_ASSERT_EQ(t.egid, 5u);
    // non-root now (gid still 0 == real, so 0 is allowed; 9 is not)
    TEST_ASSERT_EQ(sys_setgid(9, 0, 0, 0, 0, 0), -cinux::kEperm);
    TEST_ASSERT_EQ(t.egid, 5u);
}

void test_fork_inherits_via_memcpy() {
    // fork()/clone() create the child via `memcpy(child, parent, sizeof(Task))`
    // and then override only non-credential fields (tid/pid/ppid/pgid/...). A
    // whole-Task memcpy must therefore carry uid/euid/gid/egid across -- this
    // is the production inheritance path, unit-tested in isolation here.
    Task parent{};
    parent.uid  = 42;
    parent.euid = 43;
    parent.gid  = 44;
    parent.egid = 45;
    Task child{};
    __builtin_memcpy(&child, &parent, sizeof(Task));
    TEST_ASSERT_EQ(child.uid, 42u);
    TEST_ASSERT_EQ(child.euid, 43u);
    TEST_ASSERT_EQ(child.gid, 44u);
    TEST_ASSERT_EQ(child.egid, 45u);
}

}  // namespace test_creds

// ============================================================
// Entry point
// ============================================================

extern "C" void run_creds_tests() {
    TEST_SECTION("Credential Tests (F9 batch 9)");

    RUN_TEST(test_creds::test_defaults_root);
    RUN_TEST(test_creds::test_get_returns_current);
    RUN_TEST(test_creds::test_setuid_root_can_set_any);
    RUN_TEST(test_creds::test_setuid_nonroot_drop_to_real_only);
    RUN_TEST(test_creds::test_setgid_mirror);
    RUN_TEST(test_creds::test_fork_inherits_via_memcpy);

    TEST_SUMMARY();
}
