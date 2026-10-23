#include "kernel/debug/panic.hpp"

#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/debug/trace.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::debug {

namespace {

const char* state_name(cinux::proc::TaskState state) {
    switch (state) {
    case cinux::proc::TaskState::kReady:
        return "ready";
    case cinux::proc::TaskState::kRunning:
        return "running";
    case cinux::proc::TaskState::kBlocked:
        return "blocked";
    case cinux::proc::TaskState::kStopped:
        return "stopped";
    case cinux::proc::TaskState::kZombie:
        return "zombie";
    case cinux::proc::TaskState::kDead:
        return "dead";
    }
    return "unknown";
}

void print_current_task() {
    const cinux::proc::Task* current = cinux::proc::Scheduler::self().current();
    if (current == nullptr) {
        cinux::print::Println("[kern]  task none (kernel Main)");
        return;
    }
    cinux::print::Println("[kern]  task pid=%u name=%s state=%s", current->pid,
                          current->name != nullptr ? current->name : "unnamed",
                          state_name(current->state));
    if (current->vmas.holds()) {
        cinux::print::Println("[kern]  vma regions=%u", current->vmas->region_count());
        return;
    }
    cinux::print::Println("[kern]  vma none (kernel-only)");
}

void print_memory_overview() {
    const cinux::mm::Pmm&  pmm  = cinux::mm::Pmm::self();
    const cinux::mm::Heap& heap = cinux::mm::Heap::self();
    cinux::print::Println("[kern]  mem pmm free=%u managed=%u heap free=%u top=%u",
                          pmm.free_page_count(), pmm.managed_page_count(), heap.free_bytes(),
                          heap.top());
}

}  // namespace

void PanicDump(unsigned long long frame_base) {
    TraceFrom(frame_base);
    print_current_task();
    print_memory_overview();
    cinux::arch::Halt();
}


/// The stack-canary tripwire: the compiler plants a guard word from
/// this kernel's own generator at frame entry and checks it at exit;
/// a mismatch lands here, and a smashed canary is never survivable.
extern "C" [[noreturn]] void __stack_chk_fail() {
    cinux::print::Println("[panic] stack canary smashed, halting");
    cinux::arch::Halt();
}

}  // namespace cinux::debug
