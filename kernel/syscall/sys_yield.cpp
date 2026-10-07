#include "kernel/syscall/sys_yield.hpp"

#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

long long HandleYield([[maybe_unused]] unsigned long long first,
                      [[maybe_unused]] unsigned long long second,
                      [[maybe_unused]] unsigned long long third) {
    proc::Scheduler::self().yield();
    return 0;
}

}  // namespace cinux::syscall
