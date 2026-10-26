#include "kernel/syscall/sys_probe.hpp"

#include "kernel/syscall/syscall.hpp"
namespace cinux::syscall {
long long HandleUnsupportedProbe([[maybe_unused]] const SyscallFrame& frame) {
    return -kEnosys;
}
}
