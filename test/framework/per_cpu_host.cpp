#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"

namespace cinux::arch::per_cpu {

PerCpu& PerCpu::self() {
    static thread_local PerCpu local{};
    return local;
}

}

namespace cinux::arch::smp {
void NotifyRunnable() {}
}
