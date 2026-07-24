/**
 * @file kernel/test/test_flock.cpp
 * @brief In-kernel mechanism tests for flock(2) (F6-M1 B2)
 *
 * Drives do_flock_kernel directly with a stack Inode + distinct owner pointers
 * (flock uses their addresses as the lock key / owner identity -- it never
 * dereferences either). Exercises SH/EX/UN/NB, same-task upgrade, and the close
 * release hook. The blocking path (conflict without LOCK_NB) needs a second
 * schedulable task and is not exercised here -- it is covered by production use.
 */

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file_lock.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/proc/process.hpp"  // Task (identity only)
#include "kernel/syscall/sys_flock.hpp"
#include "kernel/test/big_kernel_test.h"

using cinux::fs::Inode;
using cinux::fs::FileLockManager;
using cinux::fs::kLockEx;
using cinux::fs::kLockNb;
using cinux::fs::kLockSh;
using cinux::fs::kLockUn;
using cinux::proc::Task;
using cinux::syscall::do_flock_kernel;

namespace {
// flock only compares owner identities (pointer values); it never dereferences
// them, so two distinct sentinel-backed pointers stand in for two tasks.
Task* owner_a() {
    static char buf;
    return reinterpret_cast<Task*>(&buf);
}
Task* owner_b() {
    static char buf;
    return reinterpret_cast<Task*>(&buf);
}
}  // namespace

namespace test_flock {

void test_flock_ex_exclusive_nb() {
    Inode ino{};
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockEx), 0);
    // A second task asking EX non-blocking conflicts -> EAGAIN.
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockEx | kLockNb), -cinux::kEagain);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockUn), 0);  // owner_a releases
    // Now owner_b's EX succeeds.
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockEx | kLockNb), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockUn), 0);
}

void test_flock_sh_shared() {
    Inode ino{};
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockSh), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockSh), 0);  // SH+SH coexist
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockUn), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockUn), 0);
}

void test_flock_sh_blocks_ex_nb() {
    Inode ino{};
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockSh), 0);
    // SH held by owner_a: an EX by owner_b (NB) conflicts.
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockEx | kLockNb), -cinux::kEagain);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockUn), 0);
}

void test_flock_same_task_upgrade() {
    Inode ino{};
    // A task never conflicts with itself: SH then EX (upgrade) both succeed.
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockSh), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockEx), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockUn), 0);
}

void test_flock_close_releases() {
    Inode ino{};
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_a(), kLockEx), 0);
    // The close(fd) hook drops every lock owner_a holds on the inode.
    FileLockManager::release_task_inode(&ino, owner_a());
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockEx | kLockNb), 0);
    TEST_ASSERT_EQ(do_flock_kernel(&ino, owner_b(), kLockUn), 0);
}

}  // namespace test_flock

extern "C" void run_flock_tests() {
    TEST_SECTION("flock(2) (F6-M1 B2)");
    RUN_TEST(test_flock::test_flock_ex_exclusive_nb);
    RUN_TEST(test_flock::test_flock_sh_shared);
    RUN_TEST(test_flock::test_flock_sh_blocks_ex_nb);
    RUN_TEST(test_flock::test_flock_same_task_upgrade);
    RUN_TEST(test_flock::test_flock_close_releases);
    TEST_SUMMARY();
}
