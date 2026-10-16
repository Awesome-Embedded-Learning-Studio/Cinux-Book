#include "kernel/syscall/sys_exit.hpp"

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

[[noreturn]] long long HandleExit(const SyscallFrame& frame) {
    proc::Scheduler::self().exit_current(static_cast<long long>((frame.rdi & 0xFFULL) << 8ULL));
    arch::Halt();
}

}  // namespace cinux::syscall
