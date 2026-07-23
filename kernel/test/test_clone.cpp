/**
 * @file kernel/test/test_clone.cpp
 * @brief QEMU in-kernel tests for clone() (F3-M2 batch 4)
 *
 * Mirrors test_fork_exec.cpp's harness pattern: the scheduler loop does NOT
 * run during the suite, so Scheduler::current() is nullptr by default.  Each
 * test installs a stack "tmp" task as current and calls clone() directly.
 *
 * CRITICAL: tmp.kernel_stack_top is pinned only a small offset above the live
 * RSP (not the full 16 KiB stack).  clone() copies `kernel_stack_top -
 * current_rsp` bytes into the child's 16 KiB stack; if that length exceeded
 * the stack size, child_stack_start would underflow below the stack mapping
 * and the memcpy would corrupt adjacent memory.  A small offset keeps the
 * copy well within bounds.
 *
 * Each child is remove_task()'d under an InterruptGuard so it can never be
 * scheduled (it would run fork_child_trampoline and unwind the copied stack).
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/fs/file.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/syscall/sys_futex.hpp"
#include "kernel/test/user_page.hpp"

using cinux::proc::InterruptGuard;
using cinux::proc::Scheduler;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::percpu;
using cinux::proc::task_exit_cleartid;
using cinux::syscall::sys_futex;
using cinux::proc::SharedCwd;
using cinux::proc::SharedSigActions;
using cinux::proc::Task;
using cinux::proc::clone;
using cinux::proc::signal_find_task_by_pid;

namespace {

constexpr uint64_t kCloneVm            = 0x00000100;
constexpr uint64_t kCloneFiles         = 0x00000400;
constexpr uint64_t kCloneSighand       = 0x00000800;
constexpr uint64_t kCloneThread        = 0x00010000;
constexpr uint64_t kCloneSettls        = 0x00080000;
constexpr uint64_t kCloneChildCleartid = 0x00200000;
constexpr uint64_t kCloneChildSettid   = 0x01000000;
constexpr uint64_t kCloneUserPage      = 0x0000000061100000ULL;

constexpr uint64_t kSyscallFrameSize = 128;

constexpr uint64_t FUTEX_WAIT = 0;

// A do-nothing entry for built tasks (they never truly run).
void dummy_entry() {}

uint64_t child_user_rsp(const Task* child) {
    return *reinterpret_cast<const uint64_t*>(child->kernel_stack_top - kSyscallFrameSize);
}

bool task_stack_contains(const Task* task, uint64_t addr) {
    return addr >= task->kernel_stack && addr < task->kernel_stack_top;
}

// New fork/clone child invariant (replaces the old copied_rbp_chain_is_relocated,
// which assumed the deleted RBP-chain walk).  ctx.rsp must always be in-stack;
// then the two Linux-style paths are distinguished by ctx.rip:
//   - ret_from_fork         (path A, user): rsp points at the syscall frame at top
//   - fork_child_trampoline (path B, kernel): rbp is in-stack (or 0 at top frame)
bool child_resume_context_is_valid(const Task* child) {
    if (!task_stack_contains(child, child->ctx.rsp)) {
        return false;
    }
    if (child->ctx.rip == reinterpret_cast<uint64_t>(cinux::proc::ret_from_fork)) {
        return child->ctx.rsp == child->kernel_stack_top - kSyscallFrameSize;
    }
    if (child->ctx.rip != reinterpret_cast<uint64_t>(cinux::proc::fork_child_trampoline)) {
        return false;
    }
    return child->ctx.rbp == 0 || task_stack_contains(child, child->ctx.rbp);
}

/**
 * @brief Install a stack task as current for the duration of a test.
 *
 * kernel_stack_top is pinned only 2 KiB above the live RSP so clone()'s
 * copied length (kernel_stack_top - current_rsp) stays far under the 16 KiB
 * child stack -- preventing the underflow that a full-stack offset would
 * cause.
 */
struct TmpCurrent {
    Task* prev;
    Task  tmp{};

    explicit TmpCurrent(bool with_sig = true, bool with_fd = false) {
        tmp.pid          = 42;
        tmp.ppid         = 1;
        tmp.tgid         = 42;
        tmp.group_leader = &tmp;
        if (with_sig) {
            tmp.sig_actions = SharedSigActions::create();
        }
        tmp.cwd = SharedCwd::create();
        if (with_fd) {
            tmp.fd_table = new cinux::fs::FDTable();
        }
        uint64_t rsp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
        tmp.kernel_stack_top = rsp + 2048;  // small offset -> copy length < 16 KiB stack
        prev                 = Scheduler::current();
        Scheduler::set_current(&tmp);
    }
    ~TmpCurrent() {
        Scheduler::set_current(prev);
        if (tmp.fd_table != nullptr) {
            tmp.fd_table->release();
        }
        if (tmp.sig_actions != nullptr) {
            tmp.sig_actions->release();
        }
        if (tmp.cwd != nullptr) {
            tmp.cwd->release();
        }
    }
    TmpCurrent(const TmpCurrent&)            = delete;
    TmpCurrent& operator=(const TmpCurrent&) = delete;
};

/// Prevent the freshly-created clone child from ever being scheduled.
void quarantine_child(int child_pid) {
    if (child_pid <= 0) {
        return;
    }
    Task* child = signal_find_task_by_pid(child_pid);
    if (child != nullptr) {
        Scheduler::remove_task(child);
    }
}

}  // namespace

namespace test_clone_share {

void test_clone_vm_shares_addr_space() {
    TmpCurrent cur;
    cur.tmp.addr_space = reinterpret_cast<cinux::mm::AddressSpace*>(0x1234);  // sentinel

    InterruptGuard ig;
    int            child_pid = clone(kCloneVm | kCloneThread, /*stack*/ 0x40000000ULL, 0, 0, 0);
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->addr_space, cur.tmp.addr_space);   // CLONE_VM: shared pointer
    TEST_ASSERT_EQ(child_user_rsp(child), 0x40000000ULL);    // user-RSP patched to stack
    TEST_ASSERT_TRUE(child_resume_context_is_valid(child));  // path A: ret_from_fork + frame at top
    quarantine_child(child_pid);
}

void test_clone_sighand_shares_dispositions() {
    TmpCurrent     cur(/*with_sig*/ true);
    InterruptGuard ig;
    int            child_pid = clone(kCloneVm | kCloneSighand | kCloneThread, 0, 0, 0, 0);
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->sig_actions, cur.tmp.sig_actions);  // shared, not copied
    quarantine_child(child_pid);
}

void test_clone_files_shares_fd_table() {
    TmpCurrent     cur(/*with_sig*/ true, /*with_fd*/ true);
    InterruptGuard ig;
    int            child_pid = clone(kCloneVm | kCloneFiles | kCloneThread, 0, 0, 0, 0);
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->fd_table, cur.tmp.fd_table);  // shared
    quarantine_child(child_pid);
}

}  // namespace test_clone_share

namespace test_clone_thread_group {

void test_new_process_is_own_group_leader() {
    TmpCurrent     cur;
    InterruptGuard ig;
    int            child_pid = clone(0, 0, 0, 0, 0);  // no CLONE_THREAD -> new process
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->tgid, child->pid);  // own group
    TEST_ASSERT_EQ(child->group_leader, child);
    TEST_ASSERT_EQ(child->ppid, cur.tmp.pid);  // caller is the parent
    quarantine_child(child_pid);
}

void test_clone_thread_is_sibling() {
    TmpCurrent     cur;
    InterruptGuard ig;
    Task*          saved_children = cur.tmp.children;
    int            child_pid      = clone(kCloneVm | kCloneThread, 0, 0, 0, 0);
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->tgid, cur.tmp.tgid);  // same group
    TEST_ASSERT_EQ(child->group_leader, cur.tmp.group_leader);
    TEST_ASSERT_EQ(child->ppid, cur.tmp.ppid);         // sibling: shares caller's parent
    TEST_ASSERT_EQ(cur.tmp.children, saved_children);  // NOT linked as a child
    quarantine_child(child_pid);
}

}  // namespace test_clone_thread_group

namespace test_clone_tid_tls {

void test_settls_sets_fs_base() {
    TmpCurrent     cur;
    InterruptGuard ig;
    int            child_pid = clone(kCloneVm | kCloneThread | kCloneSettls, 0, 0, 0,
                                     /*tls*/ 0x00007F0000000000ULL);
    TEST_ASSERT_GT(child_pid, 0);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->ctx.fs_base, 0x00007F0000000000ULL);  // canonical TLS base
    quarantine_child(child_pid);
}

void test_child_cleartid_settid_recorded() {
    TmpCurrent     cur;
    InterruptGuard ig;
    cinux::test::UserPage page(kCloneUserPage);
    TEST_ASSERT_TRUE(page.ok());
    uint64_t ctid = page.addr();
    int            child_pid =
        clone(kCloneVm | kCloneThread | kCloneChildCleartid | kCloneChildSettid, 0, 0, ctid, 0);
    TEST_ASSERT_GT(child_pid, 0);
    int stored_tid = 0;
    TEST_ASSERT_TRUE(page.read<int>(0, stored_tid));
    TEST_ASSERT_EQ(stored_tid, child_pid);
    Task* child = signal_find_task_by_pid(child_pid);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQ(child->clear_child_tid, ctid);  // exit zeroes + futex_wake (batch 5)
    TEST_ASSERT_EQ(child->set_child_tid, ctid);
    quarantine_child(child_pid);
}

void test_sighand_without_vm_is_einval() {
    TmpCurrent     cur;
    InterruptGuard ig;
    int            r = clone(kCloneSighand, 0, 0, 0, 0);  // SIGHAND requires VM
    TEST_ASSERT_EQ(r, -22);                               // EINVAL
}

}  // namespace test_clone_tid_tls

// ============================================================
// CLONE_CHILD_CLEARTID exit hook (batch 5)
// ============================================================

namespace test_clone_cleartid {

void test_exit_cleartid_zeros_and_wakes() {
    // The child_tid word lives in "user" memory; the exit hook must zero it
    // and futex_wake any pthread_join waiter.
    Scheduler::init();
    cinux::test::UserPage page(kCloneUserPage);
    TEST_ASSERT_TRUE(page.ok());
    TEST_ASSERT_TRUE(page.write<uint32_t>(0, 42));

    // Role-play the waiter on the harness thread: set_current + NoRescheduleGuard
    // makes block() mark it Blocked and return at once (no context switch away),
    // so we can observe the wait-queue state.
    Scheduler::NoRescheduleGuard no_resched;
    Task* waiter = TaskBuilder().set_entry(dummy_entry).set_name("clr_waiter").build();
    TEST_ASSERT_NOT_NULL(waiter);
    Scheduler::set_current(waiter);
    int64_t w = sys_futex(page.addr(), FUTEX_WAIT, 42, 0, 0, 0);
    TEST_ASSERT_EQ(w, 0);
    TEST_ASSERT_EQ(static_cast<int>(waiter->state), static_cast<int>(TaskState::Blocked));

    // The exiting thread has clear_child_tid pointing at the word.
    Task exiter{};
    exiter.clear_child_tid = page.addr();
    task_exit_cleartid(&exiter);

    uint32_t ctid_word = 99;
    TEST_ASSERT_TRUE(page.read<uint32_t>(0, ctid_word));
    TEST_ASSERT_EQ(ctid_word, 0u);  // zeroed
    TEST_ASSERT_EQ(static_cast<int>(waiter->state),
                   static_cast<int>(TaskState::Ready));  // joiner woken

    // No-op when clear_child_tid == 0.
    Task none{};
    none.clear_child_tid = 0;
    task_exit_cleartid(&none);  // must not crash
}

}  // namespace test_clone_cleartid

extern "C" void run_clone_tests() {
    TEST_SECTION("Clone Tests (F3-M2-4)");

    RUN_TEST(test_clone_share::test_clone_vm_shares_addr_space);
    RUN_TEST(test_clone_share::test_clone_sighand_shares_dispositions);
    RUN_TEST(test_clone_share::test_clone_files_shares_fd_table);

    RUN_TEST(test_clone_thread_group::test_new_process_is_own_group_leader);
    RUN_TEST(test_clone_thread_group::test_clone_thread_is_sibling);

    RUN_TEST(test_clone_tid_tls::test_settls_sets_fs_base);
    RUN_TEST(test_clone_tid_tls::test_child_cleartid_settid_recorded);
    RUN_TEST(test_clone_tid_tls::test_sighand_without_vm_is_einval);

    RUN_TEST(test_clone_cleartid::test_exit_cleartid_zeros_and_wakes);

    TEST_SUMMARY();
}
