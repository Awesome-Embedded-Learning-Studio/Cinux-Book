#include "kernel/syscall/sys_yield.hpp"

#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleYield(const SyscallFrame& /*frame*/) {
    proc::Scheduler::self().yield();
    return 0;
}

}  // namespace cinux::syscall
