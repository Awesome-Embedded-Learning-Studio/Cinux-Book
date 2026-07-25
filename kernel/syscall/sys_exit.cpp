/**
 * @file kernel/syscall/sys_exit.cpp
 * @brief sys_exit handler implementation
 *
 * Marks the current task as Dead and yields to the scheduler.
 */

#include "kernel/syscall/sys_exit.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // put_user for cleartid
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_futex.hpp"

namespace cinux::proc {

void task_exit_cleartid(Task* task) {
    if (task == nullptr || task->clear_child_tid == 0) {
        return;
    }
    // CLONE_CHILD_CLEARTID: zero the child_tid word (a user address set by
    // clone) and wake one futex waiter (the pthread_join protocol).  Use the
    // SMAP/extable accessor so a stale or unmapped user word reports EFAULT
    // instead of panicking in the exit path.
    const uint32_t zero = 0;
    if (!cinux::user::put_user(zero, reinterpret_cast<uint32_t*>(task->clear_child_tid))) {
        cinux::lib::kprintf("[EXIT] clear_child_tid write failed at %p\n",
                            reinterpret_cast<void*>(task->clear_child_tid));
        return;
    }
    cinux::syscall::futex_wake_addr(task->clear_child_tid, 1);
}

}  // namespace cinux::proc

namespace cinux::syscall {

// Terminate the current task, recording @p encoded_status (a Linux waitpid
// status word: WIFEXITED stores code<<8 with the low byte 0; WIFSIGNALED stores
// the signal number in the low byte) for the reaping parent, then yield.
// Shared by sys_exit (WIFEXITED) and signal-default-kill (WIFSIGNALED) so both
// become a reapable Zombie with the correct status encoding.  Does not return.
// (F-USABILITY batch 4 split this out of sys_exit so exit(code) and signal-kill
// encode differently; before, both stored the raw value and glibc misread
// exit(1) as WIFSIGNALED/SIGHUP.)
void exit_and_reap_current(int encoded_status) {
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        task->exit_status = encoded_status;
        // F3-M2: CLONE_CHILD_CLEARTID -- zero child_tid + wake joiner before
        // the task goes away.
        cinux::proc::task_exit_cleartid(task);
        // F3-M1: notify the parent of our exit.  SIGCHLD's default disposition
        // is Ignore, but a parent with a handler or one polling waitpid uses it.
        if (task->parent != nullptr) {
            cinux::proc::signal_send(task->parent, cinux::proc::Signal::kSigchld);
        }
        // F3-M3 batch 4a: become a Zombie (not Dead) so a wait()ing parent can
        // reap us via the children list.  Dequeue from the run queue (mirroring
        // exit_current) -- RoundRobin::pick_next() does not check state, so a
        // task left in the queue would be picked, force-set Running, and run
        // dead.  waitpid() later flips a reaped Zombie to Dead.
        task->state = cinux::proc::TaskState::Zombie;
        if (task->sched_class != nullptr) {
            task->sched_class->dequeue(task);
        }
        // F3-M3 batch 4b: wake a parent blocked in waitpid() so it re-scans
        // and reaps us (we are now Zombie).  F-QA Q4c-1 (DEBT-004): unblock
        // unconditionally -- unblock() is idempotent (no-op unless the parent
        // is actually Blocked), so this is safe even if the parent is not
        // waiting.  The old waiting_for_child gate was a plain bool read
        // cross-CPU with no barrier -> a stale read skipped the wake and
        // leaked the parent into a permanent sleep (a top flaky-hang suspect).
        if (task->parent != nullptr) {
            cinux::proc::Scheduler::unblock(task->parent);
        }
        // CLONE_VFORK: the parent was blocked in clone() until we exec or exit.
        // execve released it early; on exit we release it here and clear the
        // back-pointer so the soon-to-be-reaped Task does not dangle.  unblock()
        // is idempotent (no-op unless the target is actually Blocked).
        if (task->vfork_parent != nullptr) {
            cinux::proc::Scheduler::unblock(task->vfork_parent);
            task->vfork_parent = nullptr;
        }
        cinux::lib::kprintf("[SYSCALL] exit status=%d from tid=%u '%s'\n", encoded_status,
                            static_cast<unsigned>(task->tid), task->name);
    }

    if (cinux::proc::Scheduler::is_initialized()) {
        cinux::proc::Scheduler::yield();
    } else {
        cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
}

int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Encode as a Linux waitpid status: WIFEXITED with the code in bits 8-15
    // (low byte 0).  Storing the raw code made glibc's WIFEXITED test
    // (status & 0xff) == 0 fail for any non-zero code, so exit(1) was misread
    // as WIFSIGNALED with WTERMSIG=1 (SIGHUP) -- collect2 relayed "ld Hangup"
    // and the g++ driver ICE'd on the misreported signal.  (F-USABILITY b4.)
    exit_and_reap_current((static_cast<int>(code) & 0xff) << 8);
    return 0;
}

int64_t sys_exit_group(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // F10-M1 batch 4: musl exit() calls exit_group.  Single-threaded today,
    // so terminating the current task matches exit(); when thread groups are
    // exercised this should walk the group and reap each thread first.
    return sys_exit(code, 0, 0, 0, 0, 0);
}

}  // namespace cinux::syscall
