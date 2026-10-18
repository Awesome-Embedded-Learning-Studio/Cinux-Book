#include "kernel/syscall/sys_id.hpp"

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/// Every id answers root: no credential ledger exists yet, so a task is
/// its own superuser. The set faces answer the same way — an offer to
/// become root, from root, never fails.
long long HandleGetuid(const SyscallFrame& /*frame*/) {
    return 0;
}

long long HandleGetgid(const SyscallFrame& /*frame*/) {
    return 0;
}

long long HandleSetuid(const SyscallFrame& /*frame*/) {
    return 0;
}

long long HandleSetgid(const SyscallFrame& /*frame*/) {
    return 0;
}

long long HandleGeteuid(const SyscallFrame& /*frame*/) {
    return 0;
}

long long HandleGetegid(const SyscallFrame& /*frame*/) {
    return 0;
}

}  // namespace cinux::syscall
