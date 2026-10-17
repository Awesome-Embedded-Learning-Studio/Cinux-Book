#include "kernel/syscall/sys_gettid.hpp"

#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGettid(const SyscallFrame& /*frame*/) {
    return static_cast<long long>(CurrentTask()->pid);
}

}  // namespace cinux::syscall
