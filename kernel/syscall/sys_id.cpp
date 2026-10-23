#include "kernel/syscall/sys_id.hpp"

#include <cstdint>

#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetuid(const SyscallFrame& /*frame*/) {
    return CurrentTask()->uid;
}

long long HandleGetgid(const SyscallFrame& /*frame*/) {
    return CurrentTask()->gid;
}

long long HandleSetuid(const SyscallFrame& frame) {
    proc::Task* const kSelf       = CurrentTask();
    const auto        kWanted     = static_cast<std::uint32_t>(frame.rdi);
    const bool        kPrivileged = kSelf->euid == 0;
    if (!kPrivileged && kWanted != kSelf->uid) {
        return -kEperm;
    }
    kSelf->uid  = kWanted;
    kSelf->euid = kWanted;
    return 0;
}

long long HandleSetgid(const SyscallFrame& frame) {
    proc::Task* const kSelf       = CurrentTask();
    const auto        kWanted     = static_cast<std::uint32_t>(frame.rdi);
    const bool        kPrivileged = kSelf->egid == 0;
    if (!kPrivileged && kWanted != kSelf->gid) {
        return -kEperm;
    }
    kSelf->gid  = kWanted;
    kSelf->egid = kWanted;
    return 0;
}

long long HandleGeteuid(const SyscallFrame& /*frame*/) {
    return CurrentTask()->euid;
}

long long HandleGetegid(const SyscallFrame& /*frame*/) {
    return CurrentTask()->egid;
}

}  // namespace cinux::syscall
