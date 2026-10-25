#include "kernel/syscall/sys_sigtimedwait.hpp"

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleRtSigtimedwait(const SyscallFrame& /*frame*/) {
    return -kEagain;
}

}  // namespace cinux::syscall
