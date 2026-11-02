#include <chrono>

#include "kernel/syscall/sys_clock.hpp"

namespace cinux::syscall {

unsigned long long MonotonicNanoseconds() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
}

unsigned long long RealtimeNanoseconds() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

}
