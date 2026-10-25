#include "kernel/syscall/sys_id.hpp"

#include <cstdint>
#include <iterator>

#include "cinux/memory.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long HandleGetgroups(const SyscallFrame& frame) {
    const auto* const kSelf = CurrentTask();
    if (frame.rdi == 0) {
        return kSelf->supplementary_group_count;
    }
    if (static_cast<long long>(frame.rdi) < kSelf->supplementary_group_count) {
        return -kEinval;
    }
    const auto kBytes = kSelf->supplementary_group_count * sizeof(kSelf->supplementary_groups[0]);
    if (kBytes != 0 && !uaccess::CopyToUser(frame.rsi, kSelf->supplementary_groups, kBytes)) {
        return -kEfault;
    }
    return kSelf->supplementary_group_count;
}

long long HandleSetgroups(const SyscallFrame& frame) {
    auto* const kSelf = CurrentTask();
    if (kSelf->euid != 0) {
        return -kEperm;
    }
    if (frame.rdi > std::size(kSelf->supplementary_groups)) {
        return -kEinval;
    }
    std::uint32_t groups[16]{};
    const auto    kBytes = frame.rdi * sizeof(groups[0]);
    if (kBytes != 0 && !uaccess::CopyFromUser(groups, frame.rsi, kBytes)) {
        return -kEfault;
    }
    for (unsigned long index = 0; index < frame.rdi; ++index) {
        if (groups[index] == ~std::uint32_t{}) {
            return -kEinval;
        }
    }
    base::CopyBytes(kSelf->supplementary_groups, groups, sizeof(groups));
    kSelf->supplementary_group_count = static_cast<unsigned int>(frame.rdi);
    return 0;
}

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
