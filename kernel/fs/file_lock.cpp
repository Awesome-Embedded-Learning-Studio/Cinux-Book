/**
 * @file kernel/fs/file_lock.cpp
 * @brief FileLockManager -- flock(2) whole-file advisory locks (F6-M1 B2)
 *
 * One global lock list (Inode*, Task*, op) + one global wait queue of blocked
 * flock callers. Conflict detection is per-inode; SH+SH compatible, EX conflicts
 * with anything held by a different task. A blocking SH/EX parks on the wait
 * queue (prepare_to_wait + schedule_blocked); release/UN wake_all's so a woken
 * caller re-checks and either grants or re-blocks. The wait queue is global
 * (unrelated inodes wake too) -- coarse but correct; a per-inode queue is a
 * follow-up.
 */

#include "file_lock.hpp"

#include "kernel/errno.hpp"
#include "kernel/net/wait_queue.hpp"  // wait_enqueue / wake_all
#include "kernel/proc/process.hpp"   // Task
#include "kernel/proc/scheduler.hpp"  // prepare_to_wait / schedule_blocked
#include "kernel/proc/sync.hpp"       // Spinlock

namespace cinux::fs {

namespace {

struct LockEntry {
    Inode*      inode;
    proc::Task* owner;
    uint32_t    op;  // kLockSh or kLockEx
    LockEntry*  next;
};

LockEntry*     g_list{nullptr};     // every held lock
proc::Task*    g_waiters{nullptr};  // flock callers blocked on a conflict
proc::Spinlock g_lock;

/// Does @p want by @p owner conflict with @p held by @p holder (same inode)?
bool conflicts(uint32_t want, uint32_t held, proc::Task* owner, proc::Task* holder) {
    if (owner == holder) {
        return false;  // a task never blocks itself
    }
    if (want == kLockSh && held == kLockSh) {
        return false;  // shared readers coexist
    }
    return true;  // EX vs anything, or SH vs EX
}

bool any_conflict(Inode* inode, uint32_t want, proc::Task* owner) {
    for (LockEntry* e = g_list; e != nullptr; e = e->next) {
        if (e->inode == inode && conflicts(want, e->op, owner, e->owner)) {
            return true;
        }
    }
    return false;
}

/// Remove every lock @p owner holds on @p inode. Caller holds g_lock.
void remove_owner_inode(Inode* inode, proc::Task* owner) {
    LockEntry** pp = &g_list;
    while (*pp != nullptr) {
        if ((*pp)->inode == inode && (*pp)->owner == owner) {
            LockEntry* dead = *pp;
            *pp            = dead->next;
            delete dead;
        } else {
            pp = &(*pp)->next;
        }
    }
}

}  // namespace

void FileLockManager::release_task_inode(Inode* inode, proc::Task* owner) {
    if (inode == nullptr || owner == nullptr) {
        return;
    }
    auto g = g_lock.guard();
    remove_owner_inode(inode, owner);
    // Wake every blocked flock caller -- they re-check and either grant or
    // re-block. Coarse (unrelated inodes wake too) but correct.
    net::wake_all(g_waiters);
}

int64_t FileLockManager::flock(Inode* inode, proc::Task* owner, uint32_t operation) {
    if (inode == nullptr || owner == nullptr) {
        return -kEinval;
    }
    const uint32_t op = operation & (kLockSh | kLockEx | kLockUn);
    const bool     nb = (operation & kLockNb) != 0;

    if (op == kLockUn) {
        release_task_inode(inode, owner);
        return 0;
    }
    if (op != kLockSh && op != kLockEx) {
        return -kEinval;
    }

    for (;;) {
        {
            auto g = g_lock.guard();
            if (!any_conflict(inode, op, owner)) {
                // Grant. Replace any existing lock this owner holds on the inode
                // (SH->EX upgrade / EX->SH downgrade in one call).
                remove_owner_inode(inode, owner);
                g_list = new LockEntry{inode, owner, op, g_list};
                return 0;
            }
        }
        if (nb) {
            return -kEagain;  // EWOULDBLOCK == EAGAIN
        }
        // Block: park on the global wait queue. prepare_to_wait + enqueue are
        // atomic against a concurrent release's wake_all (both under g_lock,
        // IRQ-safe so an IRQ-time wake cannot race the Blocked transition).
        {
            auto g = g_lock.irq_guard();
            proc::Scheduler::prepare_to_wait(owner);
            net::wait_enqueue(g_waiters, owner);
        }
        proc::Scheduler::schedule_blocked();
        // EINTR: a signal landed while parked.  Unlink ourselves under g_lock
        // (a release would otherwise wake a stale link) and return -EINTR.
        if (proc::signal_deliverable_pending(owner)) {
            auto g = g_lock.irq_guard();
            net::wait_remove(g_waiters, owner);
            return -kEintr;
        }
        // Woken (a release woke us) -- loop and re-check the conflict.
    }
}

}  // namespace cinux::fs
