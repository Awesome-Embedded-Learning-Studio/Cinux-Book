/**
 * @file    init_thread.cpp
 * @brief   The init thread's two steps and the shell's whole world.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_init
 * @copyright Copyright (c) 2026
 */

#include "kernel/init/init_thread.hpp"

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

extern "C" unsigned char g_user_shell_start[];

namespace cinux::init {

namespace {

/// The shell's world: one address space, code page, stack page, and the
/// task that jumps into ring 3. Static because the kernel's only user
/// world outlives the init thread that builds it.
void launch_user_shell() {
    static mm::AddressSpace shell_space;
    base::safety::Check(shell_space.init(), "user address space failed");
    constexpr unsigned long kUserCodeBase  = 0x400000;
    constexpr unsigned long kUserStackBase = 0x500000;
    constexpr unsigned long kUserStackTop  = kUserStackBase + 4096;
    auto const              kShellPhys =
        reinterpret_cast<unsigned long>(&g_user_shell_start) - mm::kKernelImageBase;
    base::safety::Check(shell_space.map_user(kUserCodeBase, kShellPhys), "shell code page failed");
    const base::PhysAddr kStackFrame = mm::Pmm::self().allocate_page();
    base::safety::Check(kStackFrame != base::PhysAddr{}, "user stack page failed");
    base::safety::Check(shell_space.map_user(kUserStackBase, kStackFrame.raw),
                        "user stack page failed");

    auto enter_ring3 = +[]() {
        proc::Scheduler::self().current()->user_root = shell_space.root();
        asm volatile("cli" : : : "memory");
        shell_space.activate();
        JumpToRing3(kUserCodeBase, kUserStackTop);
    };
    auto* const kShell = proc::TaskBuilder{}.set_entry(enter_ring3).set_name("user_shell").build();
    base::safety::Check(kShell != nullptr, "shell task failed to build");
    proc::Scheduler::self().seat(*kShell);
}

}  // namespace

void StartInitThread() {
    auto kernel_init = +[]() {
        fs::FileWorld::self().init();
        print::Println("[kern] kernel_init: root mounted, launching shell in ring 3");
        launch_user_shell();
    };
    auto* const kInit = proc::TaskBuilder{}.set_entry(kernel_init).set_name("kernel_init").build();
    base::safety::Check(kInit != nullptr, "kernel_init task failed to build");
    proc::Scheduler::self().seat(*kInit);
}

}  // namespace cinux::init
