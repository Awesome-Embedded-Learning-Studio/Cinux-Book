#include "kernel/syscall/sys_pgrp.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

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
    const auto kPid   = pid == 0 ? CurrentTask()->pid : pid;
    long long  result = -kEsrch;
    proc::Scheduler::self().with_task(
        kPid, [&](const proc::Task& target) { result = static_cast<long long>(target.*field); });
    return result;
}

}  // namespace

long long HandleSetpgid(const SyscallFrame& frame) {
    proc::Task* const        kSelf = CurrentTask();
    const unsigned long long kPid  = frame.rdi;
    const unsigned long long kPgid = frame.rsi;
    if (static_cast<long long>(kPgid) < 0) {
        return -kEinval;
    }
    long long result = -kEsrch;
    proc::Scheduler::self().with_task(kPid == 0 ? kSelf->pid : kPid, [&](proc::Task& target) {
        if (&target != kSelf && (target.parent != kSelf || target.sid != kSelf->sid)) {
            result = -kEperm;
            return;
        }
        const auto kNewGroup = kPgid == 0 ? target.pid : kPgid;
        int        error     = 0;
        if (!group_allowed(target, kNewGroup, error)) {
            result = -error;
            return;
        }
        target.pgid = kNewGroup;
        result      = 0;
    });
    return result;
}

long long HandleSetsid(const SyscallFrame& /*frame*/) {
    proc::Task* const kSelf = CurrentTask();
    if (kSelf->pgid == kSelf->pid) {
        return -kEperm;
    }
    kSelf->sid             = kSelf->pid;
    kSelf->pgid            = kSelf->pid;
    kSelf->controlling_tty = nullptr;
    return static_cast<long long>(kSelf->pid);
}

long long HandleGetpgrp([[maybe_unused]] const SyscallFrame& frame) {
    return static_cast<long long>(CurrentTask()->pgid);
}

long long HandleGetpgid(const SyscallFrame& frame) {
    return read_identity(frame.rdi, &proc::Task::pgid);
}

long long HandleGetsid(const SyscallFrame& frame) {
    return read_identity(frame.rdi, &proc::Task::sid);
}

}  // namespace cinux::syscall
