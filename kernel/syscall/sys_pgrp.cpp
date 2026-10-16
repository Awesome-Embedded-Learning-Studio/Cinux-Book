#include "kernel/syscall/sys_pgrp.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// Resolves the setpgid target: the caller itself, or a named pid
/// that must be one of the caller's own children still sharing its
/// session. nullptr with a written errno when the name points
/// nowhere or at a stranger.
proc::Task* resolve_move_target(proc::Task& caller, unsigned long long pid, int& error) {
    if (pid == 0) {
        return &caller;
    }
    proc::Task* const kTarget = proc::Scheduler::self().find_by_pid(pid);
    if (kTarget == nullptr) {
        error = kEsrch;
        return nullptr;
    }
    if (kTarget->parent != &caller || kTarget->sid != caller.sid) {
        error = kEperm;
        return nullptr;
    }
    return kTarget;
}

/// Checks the group a target is being moved into: the target's own
/// pid founds a fresh group, and any other value must already name a
/// group inside the target's session — a group lives as long as any
/// member still carries its id, leader or not. False with a written
/// errno when nothing in the session answers to the name.
bool group_allowed(const proc::Task& target, unsigned long long pgid, int& error) {
    if (pgid == target.pid) {
        return true;
    }
    bool found = false;
    proc::Scheduler::self().for_each_task([&found, &target, pgid](const proc::Task& member) {
        found = found || (member.pgid == pgid && member.sid == target.sid);
    });
    if (!found) {
        error = kEinval;
    }
    return found;
}

/// Shared read half of getpgid and getsid: one identity word off a
/// named task, zero naming the caller. Answers a negative errno when
/// the name points nowhere.
long long read_identity(unsigned long long pid, const unsigned long long proc::Task::* field) {
    const proc::Task* const kTarget =
        pid == 0 ? CurrentTask() : proc::Scheduler::self().find_by_pid(pid);
    if (kTarget == nullptr) {
        return -kEsrch;
    }
    return static_cast<long long>(kTarget->*field);
}

}  // namespace

long long HandleSetpgid(const SyscallFrame& frame) {
    proc::Task* const        kSelf = CurrentTask();
    const unsigned long long kPid  = frame.rdi;
    const unsigned long long kPgid = frame.rsi;
    if (static_cast<long long>(kPgid) < 0) {
        return -kEinval;
    }
    int               error   = 0;
    proc::Task* const kTarget = resolve_move_target(*kSelf, kPid, error);
    if (kTarget == nullptr) {
        return -error;
    }
    const unsigned long long kNewGroup = kPgid == 0 ? kTarget->pid : kPgid;
    if (!group_allowed(*kTarget, kNewGroup, error)) {
        return -error;
    }
    kTarget->pgid = kNewGroup;
    return 0;
}

long long HandleSetsid(const SyscallFrame& /*frame*/) {
    proc::Task* const kSelf = CurrentTask();
    if (kSelf->pgid == kSelf->pid) {
        return -kEperm;
    }
    kSelf->sid  = kSelf->pid;
    kSelf->pgid = kSelf->pid;
    return static_cast<long long>(kSelf->pid);
}

long long HandleGetpgid(const SyscallFrame& frame) {
    return read_identity(frame.rdi, &proc::Task::pgid);
}

long long HandleGetsid(const SyscallFrame& frame) {
    return read_identity(frame.rdi, &proc::Task::sid);
}

}  // namespace cinux::syscall
