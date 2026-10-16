#include "kernel/syscall/sys_getpid.hpp"

#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetpid(const SyscallFrame& /*frame*/) {
    return static_cast<long long>(CurrentTask()->pid);
}

}  // namespace cinux::syscall
