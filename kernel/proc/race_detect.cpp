/**
 * @file kernel/proc/race_detect.cpp
 * @brief SMP data-race watchpoint detector implementation (CINUX_RACE_DETECT)
 *
 * Linked only under CINUX_RACE_DETECT (§14 file gate: OFF links
 * race_detect_stub.cpp).  See race_detect.hpp for the model.
 */

#include "kernel/proc/race_detect.hpp"

#include "kernel/arch/x86_64/backtrace.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/percpu.hpp"

namespace cinux::proc {

bool race_check_access_probe(RaceWatchpoint& w) {
    const uint32_t cpu  = percpu()->cpu_id;
    const uint32_t prev = __atomic_exchange_n(&w.last_cpu, cpu, __ATOMIC_ACQ_REL);
    return (prev != kRaceCpuNone && prev != cpu);
}

void race_check_access(RaceWatchpoint& w) {
    const uint32_t cpu  = percpu()->cpu_id;
    const uint32_t prev = __atomic_exchange_n(&w.last_cpu, cpu, __ATOMIC_ACQ_REL);
    if (prev != kRaceCpuNone && prev != cpu) {
        // Offender stack: backtrace() walks from THIS frame's caller, i.e. the
        // RACE_TOUCH site -- the very access that raced.
        cinux::arch::backtrace();
        cinux::lib::kpanic("[SMP-RACE] %s: cpu%u touched after cpu%u without lock",
                           w.name, static_cast<unsigned>(cpu),
                           static_cast<unsigned>(prev));
    }
}

}  // namespace cinux::proc
