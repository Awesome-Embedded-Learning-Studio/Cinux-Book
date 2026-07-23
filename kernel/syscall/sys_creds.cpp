/**
 * @file kernel/syscall/sys_creds.cpp
 * @brief Process-credential syscall handlers (F9 batch 9 / M3)
 *
 * @see sys_creds.hpp for the simplified setuid/setgid rule and the F6 deferral
 * of setuid-binary / saved-set semantics.
 */

#include "kernel/syscall/sys_creds.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

using cinux::proc::Scheduler;
using cinux::proc::Task;

int64_t sys_getuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    return (task == nullptr) ? 0 : static_cast<int64_t>(task->uid);
}

int64_t sys_geteuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    return (task == nullptr) ? 0 : static_cast<int64_t>(task->euid);
}

int64_t sys_getgid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    return (task == nullptr) ? 0 : static_cast<int64_t>(task->gid);
}

int64_t sys_getegid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    return (task == nullptr) ? 0 : static_cast<int64_t>(task->egid);
}

// Simplified POSIX setuid (see header): root (euid==0) sets euid freely; a
// non-root task may only drop euid back to its real uid. Returns 0 / -EPERM.
int64_t sys_setuid(uint64_t uid_arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    if (task == nullptr) {
        return -kEperm;
    }
    const uint32_t new_uid = static_cast<uint32_t>(uid_arg);
    if (task->euid == 0 || new_uid == task->uid) {
        task->euid = new_uid;
        return 0;
    }
    return -kEperm;
}

int64_t sys_setgid(uint64_t gid_arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = Scheduler::current();
    if (task == nullptr) {
        return -kEperm;
    }
    const uint32_t new_gid = static_cast<uint32_t>(gid_arg);
    if (task->egid == 0 || new_gid == task->gid) {
        task->egid = new_gid;
        return 0;
    }
    return -kEperm;
}

// F-ECO batch 8: supplementary groups.  do_*_kernel take an explicit Task* (so
// they are unit-testable with a stack Task -- the test kernel's main thread has
// Scheduler::current()==null); sys_getgroups/setgroups resolve current() and
// cross the user boundary (copy_to/from_user) via a kernel staging buffer.
int64_t do_getgroups_kernel(const Task* task, uint32_t* out_groups, uint32_t cap) {
    if (task == nullptr) {
        return 0;
    }
    if (out_groups == nullptr) {
        return static_cast<int64_t>(task->ngroups);  // count query (mirrors size==0)
    }
    if (cap < task->ngroups) {
        return -kEinval;  // buffer too small
    }
    for (uint32_t i = 0; i < task->ngroups; ++i) {
        out_groups[i] = task->groups[i];
    }
    return static_cast<int64_t>(task->ngroups);
}

int64_t sys_getgroups(uint64_t size, uint64_t list_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    Task* task = Scheduler::current();
    if (task == nullptr) {
        return 0;
    }
    if (size == 0) {
        return static_cast<int64_t>(task->ngroups);  // Linux: size==0 -> return count
    }
    // do_ fills a kernel staging buffer (capped at NGROUPS_MAX); then copy out.
    uint32_t kbuf[Task::kNGroupsMax];
    int64_t  n = do_getgroups_kernel(task, kbuf, static_cast<uint32_t>(size));
    if (n < 0) {
        return n;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(list_virt), kbuf,
                                   static_cast<uint32_t>(n) * sizeof(uint32_t))) {
        return -kEfault;
    }
    return n;
}

int64_t do_setgroups_kernel(Task* task, const uint32_t* in_groups, uint32_t count) {
    if (task == nullptr) {
        return -kEperm;
    }
    if (task->euid != 0) {
        return -kEperm;  // root-only (CAP_SETGID stand-in); hobby simplification
    }
    if (count > Task::kNGroupsMax) {
        return -kEinval;  // too many groups
    }
    if (count > 0 && in_groups == nullptr) {
        return -kEfault;
    }
    for (uint32_t i = 0; i < count; ++i) {
        task->groups[i] = in_groups[i];
    }
    task->ngroups = count;
    return 0;
}

int64_t sys_setgroups(uint64_t size, uint64_t list_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    Task* task = Scheduler::current();
    if (task == nullptr) {
        return -kEperm;
    }
    if (size > Task::kNGroupsMax) {
        return -kEinval;  // early bounds check before staging the user array
    }
    uint32_t kbuf[Task::kNGroupsMax];
    if (size > 0) {
        if (!cinux::user::copy_from_user(kbuf, reinterpret_cast<void*>(list_virt),
                                         size * sizeof(uint32_t))) {
            return -kEfault;
        }
    }
    return do_setgroups_kernel(task, kbuf, static_cast<uint32_t>(size));
}

}  // namespace cinux::syscall
