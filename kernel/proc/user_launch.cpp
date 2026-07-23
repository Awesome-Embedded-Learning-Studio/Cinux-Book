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
#include "kernel/proc/initial_stack.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

void enter_loaded_program(const char* path, const char* const argv[], const char* const envp[],
                          const ElfAuxInfo& elf_aux) {
    auto* task = Scheduler::current();

    // User stack: pre-map the top USER_STACK_PAGES, then record the full
    // demand-growth Stack VMA under the F2-M5 hard gate. Accesses below
    // [USER_STACK_TOP - USER_STACK_GROWTH) hit no VMA -> segfault (guard).
    uint64_t entry = task->ctx.rip;

    // F9 batch 8 (ASLR): randomize the user stack top (page-aligned, bounded by
    // the 1 MiB demand-growth region). One value feeds pre-map, VMA, and the
    // initial-stack build so they all agree.
    const uint64_t stack_top = cinux::arch::USER_STACK_TOP - cinux::lib::aslr_stack_offset();

    constexpr uint64_t kUserPageFlags =
        cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_USER;
    uint64_t stack_base    = stack_top - cinux::arch::USER_STACK_PAGES * cinux::arch::PAGE_SIZE;
    uint64_t top_page_phys = 0;  // the page containing stack_top (we write the entry stack here)

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
        if (virt == stack_top - cinux::arch::PAGE_SIZE) {
            top_page_phys = phys;
        }
    }

    constexpr cinux::mm::VmaFlags kStackVma =
        cinux::mm::VmaFlags::Read | cinux::mm::VmaFlags::Write | cinux::mm::VmaFlags::Stack;
    const uint64_t kStackVmaStart = stack_top - cinux::arch::USER_STACK_GROWTH;
    if (!task->addr_space->vmas().insert(kStackVmaStart, stack_top, kStackVma).ok()) {
        cinux::lib::kprintf("[PROC] stack VMA record failed\n");
        Scheduler::exit_current();
    }

    // Build the Linux x86_64 initial stack (argc/argv/envp/auxv) into the top
    // stack page via the direct map. musl's __init_libc reads AT_PHDR/PHNUM/
    // PHENT (program headers), AT_PAGESZ, AT_RANDOM (SSP canary), AT_UID/EUID/
    // GID/EGID + AT_SECURE, AT_ENTRY here at startup. Entry RSP = stack_top -
    // size, 16-byte aligned (SysV process-entry convention).
    uint64_t argc = 0;
    while (argv != nullptr && argv[argc] != nullptr) {
        ++argc;
    }
    uint64_t envc = 0;
    while (envp != nullptr && envp[envc] != nullptr) {
        ++envc;
    }

    uint8_t  random16[16];
    uint64_t r0 = cinux::lib::g_random.next64();
    uint64_t r1 = cinux::lib::g_random.next64();
    for (int i = 0; i < 8; ++i) {
        random16[i]     = static_cast<uint8_t>(r0 >> (8 * i));
        random16[8 + i] = static_cast<uint8_t>(r1 >> (8 * i));
    }

    const bool     secure = (task->euid != task->uid) || (task->egid != task->gid);
    const AuxEntry auxv[] = {
        {AT_PHDR, elf_aux.at_phdr},
        {AT_PHNUM, elf_aux.at_phnum},
        {AT_PHENT, elf_aux.at_phent},
        {AT_PAGESZ, cinux::arch::PAGE_SIZE},
        {AT_BASE, elf_aux.at_base},  // F10-M2: interpreter load base (0 if static)
        {AT_FLAGS, 0},
        {AT_ENTRY, elf_aux.at_entry},
        {AT_UID, task->uid},
        {AT_EUID, task->euid},
        {AT_GID, task->gid},
        {AT_EGID, task->egid},
        {AT_HWCAP, 0},
        {AT_CLKTCK, 100},
        {AT_SECURE, secure ? 1ULL : 0ULL},
    };

    auto*    page = reinterpret_cast<uint8_t*>(top_page_phys + cinux::arch::DIRECT_MAP_BASE);
    uint64_t size = build_initial_stack(page, cinux::arch::PAGE_SIZE, stack_top, argc, argv, envc,
                                        envp, auxv, 14, path, random16);
    if (size == 0) {
        cinux::lib::kprintf("[PROC] initial stack build failed (overflow)\n");
        Scheduler::exit_current();
    }
    uint64_t user_rsp = stack_top - size;

    cinux::lib::kprintf("[PROC] jumping to user mode: entry=%p rsp=%p stack_top=%p\n",
                        reinterpret_cast<void*>(entry), reinterpret_cast<void*>(user_rsp),
                        reinterpret_cast<void*>(stack_top));

    task->addr_space->activate();
    update_syscall_stack(task->kernel_stack_top);
    jump_to_usermode(entry, user_rsp, 0);
    Scheduler::exit_current();  // unreachable; jump_to_usermode does not return
}

void launch_user_program(const char* path, const char* const argv[], const char* const envp[]) {
    ElfAuxInfo elf_aux{};
    auto       result = execve(path, argv, envp, &elf_aux);
    if (result != ExecveResult::Ok) {
        cinux::lib::kprintf("[PROC] execve(%s) failed: %d\n", path, static_cast<int>(result));
        Scheduler::exit_current();
    }
    enter_loaded_program(path, argv, envp, elf_aux);
    Scheduler::exit_current();  // unreachable; enter_loaded_program jumps
}

}  // namespace cinux::proc
