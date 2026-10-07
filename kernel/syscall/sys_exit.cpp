#include "kernel/syscall/sys_exit.hpp"

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

[[noreturn]] long long HandleExit([[maybe_unused]] unsigned long long first,
                                  [[maybe_unused]] unsigned long long second,
                                  [[maybe_unused]] unsigned long long third) {
    proc::Scheduler::self().exit_current();
    arch::Halt();
}

}  // namespace cinux::syscall
