/**
 * @file kernel/proc/user_launch.cpp
 * @brief launch_user_program() — shared execve + user stack + jump sequence
 *
 * Extracted from the previously-duplicated code in kernel/proc/init.cpp
 * (non-GUI shell fork path) and kernel/gui/gui_init.cpp (shell_child_entry).
 * Both sites now install addr_space (+ optional FDTable) then delegate here.
 */

#include "kernel/proc/user_launch.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/lib/aslr.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/execve.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

void launch_user_program(const char* path, const char* const argv[], const char* const envp[]) {
    auto* task   = Scheduler::current();
    auto  result = execve(path, argv, envp);
    if (result != ExecveResult::Ok) {
        cinux::lib::kprintf("[PROC] execve(%s) failed: %d\n", path, static_cast<int>(result));
        Scheduler::exit_current();
    }

    // User stack: pre-map the top USER_STACK_PAGES, then record the full
    // demand-growth Stack VMA under the F2-M5 hard gate. Accesses below
    // [USER_STACK_TOP - USER_STACK_GROWTH) hit no VMA -> segfault (guard).
    uint64_t entry = task->ctx.rip;

    // F9 batch 8 (ASLR): randomize the user stack top. The offset is page-
    // aligned (so USER_ABI_RSP_OFFSET still yields %16==8) and bounded by the
    // 1 MiB demand-growth region, keeping the overflow guard page trapping.
    // One value feeds all four stack sites so pre-map, VMA, and jump RSP agree.
    const uint64_t stack_top = cinux::arch::USER_STACK_TOP - cinux::lib::aslr_stack_offset();

    constexpr uint64_t kUserPageFlags =
        cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_USER;
    uint64_t stack_base = stack_top - cinux::arch::USER_STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < cinux::arch::USER_STACK_PAGES; i++) {
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[PROC] user stack alloc failed\n");
            Scheduler::exit_current();
        }
        uint64_t virt = stack_base + i * cinux::arch::PAGE_SIZE;
        if (!task->addr_space->map(virt, phys, kUserPageFlags)) {
            cinux::lib::kprintf("[PROC] user stack map failed at %p\n",
                                reinterpret_cast<void*>(virt));
            Scheduler::exit_current();
        }
    }

    constexpr cinux::mm::VmaFlags kStackVma =
        cinux::mm::VmaFlags::Read | cinux::mm::VmaFlags::Write | cinux::mm::VmaFlags::Stack;
    const uint64_t kStackVmaStart = stack_top - cinux::arch::USER_STACK_GROWTH;
    if (!task->addr_space->vmas().insert(kStackVmaStart, stack_top, kStackVma).ok()) {
        cinux::lib::kprintf("[PROC] stack VMA record failed\n");
        Scheduler::exit_current();
    }

    cinux::lib::kprintf("[PROC] jumping to user mode: entry=%p stack_top=%p\n",
                        reinterpret_cast<void*>(entry), reinterpret_cast<void*>(stack_top));

    task->addr_space->activate();
    update_syscall_stack(task->kernel_stack_top);
    jump_to_usermode(entry, stack_top - cinux::arch::USER_ABI_RSP_OFFSET, 0);
    Scheduler::exit_current();  // unreachable; jump_to_usermode does not return
}

}  // namespace cinux::proc
