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
#include <utility>

#include "cinux/assert.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/tty/console_tty.hpp"
#include "kernel/fs/devfs/console_node.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::init {

namespace {


/// The loaded shell's entry and root, staged for the capture-free trampoline.
std::uint64_t g_shell_entry = 0;
unsigned long g_shell_root  = 0;

/// Reads one whole file off the mounted volume into an owned buffer,
/// handing back the resolved view for the caller's own lookups.
bool read_volume_file(const char* path, base::Own<unsigned char[]>& out, unsigned long& out_bytes,
                      fs::FsView& view) {
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return false;
    }
    view = kView.value();
    return fs::ReadWholeFile(view, out, out_bytes).ok();
}

/// One built shell world: the space and its range register, seat-ready.
struct ShellWorld {
    base::Shared<mm::AddressSpace> space;
    base::Shared<mm::VmaStore>     vmas;
};

/// Loads the volume's shell ELF into a fresh world with a stack page and a
/// full range register on top; an empty ShellWorld when any step refuses.
ShellWorld build_shell_world(std::uint64_t& entry) {
    unsigned long              program_bytes = 0;
    base::Own<unsigned char[]> program;
    fs::FsView                 view{.ops = nullptr, .self = nullptr, .rest = {}};
    if (!read_volume_file("/bin/sh", program, program_bytes, view)) {
        print::Println("[kern] kernel_init: /bin/sh unreadable on the volume");
        return {};
    }
    base::Shared<mm::AddressSpace> space(new mm::AddressSpace{});
    base::Shared<mm::VmaStore>     vmas(new mm::VmaStore{});
    if (!space.holds() || !vmas.holds() || !space->init()) {
        return {};
    }
    const base::Result<fs::FsNode> kNode = view.ops->lookup(view.self, view.rest);
    if (!kNode.ok()) {
        return {};
    }
    const loader::LoadWorld kWorld{
        .space     = *space,
        .vmas      = *vmas,
        .file      = kNode.value(),
        .file_ops  = view.ops,
        .file_self = view.self,
    };
    loader::EntryFacts facts{};
    if (!loader::LoadElfIntoSpace(program.get(), program_bytes, kWorld, facts)) {
        return {};
    }
    entry            = facts.program_entry;
    facts.page_bytes = cinux::arch::page::kSize;
    vmas->seed_heap(cinux::base::math::AlignUp(vmas->max_end(), cinux::arch::page::kSize),
                    mm::kUserBrkMmapSplit);
    mm::VmaRegion stack_range{};
    stack_range.start = proc::kUserProgramStackTop - mm::kUserStackBytes - cinux::arch::page::kSize;
    stack_range.end   = proc::kUserProgramStackTop;
    stack_range.kind  = mm::VmaKind::kStack;
    stack_range.readable = true;
    stack_range.writable = true;
    if (!vmas->register_range(stack_range).ok()) {
        return {};
    }
    const mm::Pmm::PhysAddr kStackFrame = mm::Pmm::self().allocate_page();
    if (kStackFrame == mm::Pmm::PhysAddr{}) {
        return {};
    }
    cinux::base::SetBytes(cinux::base::PtrAt<unsigned char>(mm::DirectMapVirt(kStackFrame.raw)), 0,
                          cinux::arch::page::kSize);
    const char* const       kShellArgv[2] = {"/bin/sh", nullptr};
    const char* const       kShellEnvp[2] = {"PATH=/bin:/usr/bin", nullptr};
    const loader::EntryRuns kShellRuns{.arguments = kShellArgv, .environment = kShellEnvp};
    loader::WriteEntryPicture(cinux::base::PtrAt<unsigned char>(mm::DirectMapVirt(kStackFrame.raw)),
                              proc::kUserProgramStackTop - proc::kUserEntryStackOffset, kShellRuns,
                              facts);
    if (!space->map_user(proc::kUserProgramStackTop - cinux::arch::page::kSize, kStackFrame.raw)) {
        mm::Pmm::self().free_page(kStackFrame);
        return {};
    }
    if (!proc::MapSignalTrampoline(*space, *vmas)) {
        return {};
    }
    return ShellWorld{.space = std::move(space), .vmas = std::move(vmas)};
}

/// Seats one shell task over a freshly built world; the task owns the world
/// and its range register from here to their reclaim. False when the world
/// refused to build.
bool seat_shell_task(proc::Task& parent) {
    std::uint64_t entry = 0;
    ShellWorld    world = build_shell_world(entry);
    if (!world.space.holds() || !world.vmas.holds()) {
        return false;
    }
    g_shell_entry    = entry;
    g_shell_root     = world.space->root();
    auto enter_ring3 = +[] {
        proc::Task* const kSelf = proc::Scheduler::self().current();
        kSelf->user_root        = g_shell_root;
        asm volatile("cli" : : : "memory");
        cinux::arch::LoadCr3(g_shell_root);
        JumpToRing3(g_shell_entry, proc::kUserProgramStackTop - proc::kUserEntryStackOffset);
    };
    auto* const kShell = proc::TaskBuilder{}
                             .set_entry(enter_ring3)
                             .set_name("user_shell")
                             .set_parent(parent)
                             .build();
    if (kShell == nullptr) {
        return false;
    }
    kShell->space = std::move(world.space);
    kShell->vmas  = std::move(world.vmas);
    if (!fs::FileWorld::self().seat_console_streams(*kShell->files).ok()) {
        return false;
    }
    proc::Scheduler::self().seat(*kShell, false);
    kShell->pgid            = kShell->pid;
    kShell->sid             = kShell->pid;
    kShell->controlling_tty = &driver::ConsoleTty();
    fs::AdoptConsoleForeground(kShell->pgid);
    cinux::base::CopyBytes(kShell->exe, "/bin/sh", 8);
    proc::Scheduler::self().unblock(*kShell);
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
            proc::Scheduler::self().wait_child(-1, false, receipt);
            print::Println("[kern] kernel_init: shell exited with %lld, respawning", receipt.code);
        }
    };
    auto* const kInit = proc::TaskBuilder{}.set_entry(kernel_init).set_name("kernel_init").build();
    base::safety::Check(kInit != nullptr, "kernel_init task failed to build");
    proc::Scheduler::self().seat(*kInit, false);
    kInit->pgid = kInit->pid;
    kInit->sid  = kInit->pid;
    proc::Scheduler::self().unblock(*kInit);
}

}  // namespace cinux::init
