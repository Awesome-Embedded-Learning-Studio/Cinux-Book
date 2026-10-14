/**
 * @file    init_thread.cpp
 * @brief   The init thread: mount the volume, keep a shell alive.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_init
 * @copyright Copyright (c) 2026
 */

#include "kernel/init/init_thread.hpp"

#include <cstdint>

#include "cinux/assert.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::init {

namespace {


/// The loaded shell's entry and root, staged for the capture-free trampoline.
std::uint64_t g_shell_entry = 0;
unsigned long g_shell_root  = 0;

/// Reads one whole file off the mounted volume into an owned buffer.
bool read_volume_file(const char* path, base::Own<unsigned char[]>& out, unsigned long& out_bytes) {
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    return kView.ok() && fs::ReadWholeFile(kView.value(), out, out_bytes);
}

/// Loads the volume's shell ELF into a fresh world with a stack page on top;
/// nullptr with everything returned when any step refuses.
base::Own<mm::AddressSpace> build_shell_world(std::uint64_t& entry) {
    unsigned long              program_bytes = 0;
    base::Own<unsigned char[]> program;
    if (!read_volume_file("/bin/sh", program, program_bytes)) {
        print::Println("[kern] kernel_init: /bin/sh unreadable on the volume");
        return {};
    }
    base::Own<mm::AddressSpace> space(new mm::AddressSpace{});
    if (!space.holds() || !space->init() ||
        !loader::LoadElfIntoSpace(program.get(), program_bytes, *space, entry)) {
        return {};
    }
    const mm::Pmm::PhysAddr kStackFrame = mm::Pmm::self().allocate_page();
    if (kStackFrame == mm::Pmm::PhysAddr{}) {
        return {};
    }
    if (!space->map_user(proc::kUserProgramStackTop - cinux::arch::page::kSize, kStackFrame.raw)) {
        mm::Pmm::self().free_page(kStackFrame);
        return {};
    }
    return space;
}

/// Seats one shell task over a freshly built world; the task owns the world
/// from here to its reclaim. False when the world refused to build.
bool seat_shell_task(proc::Task& parent) {
    std::uint64_t               entry = 0;
    base::Own<mm::AddressSpace> space = build_shell_world(entry);
    if (!space.holds()) {
        return false;
    }
    g_shell_entry    = entry;
    g_shell_root     = space->root();
    auto enter_ring3 = +[] {
        proc::Task* const kSelf = proc::Scheduler::self().current();
        kSelf->user_root        = g_shell_root;
        asm volatile("cli" : : : "memory");
        cinux::arch::LoadCr3(g_shell_root);
        JumpToRing3(g_shell_entry, proc::kUserProgramStackTop);
    };
    auto* const kShell = proc::TaskBuilder{}
                             .set_entry(enter_ring3)
                             .set_name("user_shell")
                             .set_parent(parent)
                             .build();
    if (kShell == nullptr) {
        return false;
    }
    kShell->space = space.release();
    proc::Scheduler::self().seat(*kShell);
    return true;
}

}  // namespace

void StartInitThread() {
    auto kernel_init = +[] {
        fs::FileWorld::self().init();
        print::Println("[kern] kernel_init: root mounted, launching shell from the volume");
        for (;;) {
            proc::Task* const kSelf = proc::Scheduler::self().current();
            if (!seat_shell_task(*kSelf)) {
                print::Println("[kern] kernel_init: shell refused, parking");
                return;
            }
            proc::ChildReceipt receipt{};
            proc::Scheduler::self().wait_child(-1, receipt);
            print::Println("[kern] kernel_init: shell exited with %lld, respawning", receipt.code);
        }
    };
    auto* const kInit = proc::TaskBuilder{}.set_entry(kernel_init).set_name("kernel_init").build();
    base::safety::Check(kInit != nullptr, "kernel_init task failed to build");
    proc::Scheduler::self().seat(*kInit);
}

}  // namespace cinux::init
