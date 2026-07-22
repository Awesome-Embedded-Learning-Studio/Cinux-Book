/**
 * @file kernel/syscall/sys_creds.cpp
 * @brief Process-credential syscall handlers (F9 batch 9 / M3)
 *
 * @see sys_creds.hpp for the simplified setuid/setgid rule and the F6 deferral
 * of setuid-binary / saved-set semantics.
 */

#include "kernel/syscall/sys_creds.hpp"

#include "kernel/errno.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

using cinux::proc::Scheduler;

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

}  // namespace cinux::syscall
