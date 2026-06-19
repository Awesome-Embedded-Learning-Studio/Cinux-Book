/**
 * @file kernel/arch/x86_64/backtrace.cpp
 * @brief Frame-pointer-based backtrace implementation
 *
 * The walk is split into backtrace_capture() (pure: fills an address array,
 * unit-testable) and backtrace_from() (prints via KALLSYMS).  read_frame()
 * gates every dereference behind a kernel-stack-range check so a corrupt or
 * wild frame pointer cannot trigger a halting kernel #PF.
 *
 * GOTCHA: range-checking (not VMM::translate) is used because translate() does
 * not support huge pages -- it returns 0 for 2 MB entries -- and the boot/test
 * stack lives in a huge-mapped region, which made a translate-based walk stop
 * on the very first frame.  Range checks also avoid the VMM spinlock, so the
 * walk cannot deadlock if a panic occurs with the VMM lock held.  Trade-off:
 * only the current task kernel stack and the boot stack are walked -- a panic
 * on the IST1 double-fault stack would not resolve (rare; future hardening).
 */

#include "kernel/arch/x86_64/backtrace.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/lib/kallsyms.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/scheduler.hpp"

extern "C" char __kernel_stack_top[];
extern "C" char __boot_guard_end[];

namespace cinux::arch {

using cinux::lib::kprintf;

namespace {

// True if addr lies in a kernel stack we are willing to walk: the current
// task's kernel stack (runtime panics) or the boot stack (early boot / tests).
bool in_kernel_stack(uint64_t addr) {
    auto* t = cinux::proc::Scheduler::current();
    if (t != nullptr && t->kernel_stack != 0) {
        if (addr >= t->kernel_stack && addr < t->kernel_stack_top) {
            return true;
        }
    }
    const uint64_t bot = reinterpret_cast<uint64_t>(__boot_guard_end);
    const uint64_t top = reinterpret_cast<uint64_t>(__kernel_stack_top);
    if (addr >= bot && addr < top) {
        return true;
    }
    return false;
}

// Read the saved-RBP (slot +0) and return-address (slot +8) of the frame at
// @p rbp, but only after confirming both slots lie in a kernel stack.  Returns
// false otherwise so the walker stops cleanly instead of faulting.
bool read_frame(uint64_t rbp, uint64_t& next_rbp, uint64_t& ret_addr) {
    if (rbp == 0) {
        return false;
    }
    if (!in_kernel_stack(rbp) || !in_kernel_stack(rbp + 8)) {
        return false;
    }
    const volatile uint64_t* f = reinterpret_cast<const volatile uint64_t*>(rbp);
    next_rbp                   = f[0];
    ret_addr                   = f[1];
    return true;
}

}  // namespace

size_t backtrace_capture(uint64_t rbp, uint64_t* addrs, size_t max) {
    if (addrs == nullptr || max == 0) {
        return 0;
    }
    size_t   count = 0;
    uint64_t cur   = rbp;
    while (count < max) {
        uint64_t next_rbp = 0;
        uint64_t ret_addr = 0;
        if (!read_frame(cur, next_rbp, ret_addr)) {
            break;
        }
        addrs[count++] = ret_addr;
        // The chain ends at the top frame (next RBP == 0) and must strictly
        // grow upward (x86 stacks grow down); otherwise it is corrupt or
        // circular -- stop rather than walk wild pointers.
        if (next_rbp == 0 || next_rbp <= cur) {
            break;
        }
        cur = next_rbp;
    }
    return count;
}

void backtrace_from(uint64_t rbp, size_t max_frames) {
    if (max_frames == 0 || max_frames > kBacktraceMaxFrames) {
        max_frames = kBacktraceMaxFrames;
    }
    uint64_t addrs[kBacktraceMaxFrames];
    size_t   n = backtrace_capture(rbp, addrs, max_frames);
    kprintf("Backtrace (%u frames):\n", static_cast<unsigned>(n));
    char buf[96];
    for (size_t i = 0; i < n; i++) {
        cinux::lib::kallsyms_lookup(addrs[i], buf, sizeof(buf));
        kprintf("  [%u] %p  %s\n", static_cast<unsigned>(i), reinterpret_cast<void*>(addrs[i]),
                buf);
    }
}

void backtrace() {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    backtrace_from(rbp, 0);
}

}  // namespace cinux::arch
