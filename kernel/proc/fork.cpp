#include "kernel/proc/fork.hpp"

#include <utility>

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"

extern "C" {

/**
 * @brief         The child side of a fork, entered only once per fork.
 * @note          The mirror of the syscall return tail: unwinds the
 *                entry frame synthesized on the child's kernel stack
 *                and leaves for ring 3 through sysretq with rax zero,
 *                which is the whole difference between parent and
 *                child from the caller's chair.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void ForkChildEntry();
}

namespace cinux::proc {

namespace {

/// A spawn refused before it starts beats one that dies halfway down
/// the kernel stack: the spawner's own call chain must sit well inside
/// the stack it is about to hand away, or the frames it touches while
/// assembling the child can run off the end.
void guard_spawn_depth(const Task& spawner) {
    unsigned long long rsp = 0;  // NOLINT(misc-const-correctness) written by the asm output
    asm volatile("movq %%rsp, %0" : "=r"(rsp));
    const unsigned long long kUsed = spawner.stack_base + kStackBytes - rsp;
    cinux::base::safety::Check(kUsed < (kStackBytes / 4) * 3,
                               "spawn refused: the caller's kernel stack is nearly spent");
}

}  // namespace

Task* BuildForkChild(unsigned long long frame_address) {
    const arch::IrqGuard kEntryGuard;
    const auto& parent_frame = *cinux::base::PtrAt<const syscall::SyscallFrame>(frame_address);
    const unsigned long long kUserR12  = parent_frame.r12;
    const unsigned long long kUserR13  = parent_frame.r13;
    const unsigned long long kUserR14  = parent_frame.r14;
    const unsigned long long kUserR15  = parent_frame.r15;
    const auto&              scheduler = Scheduler::self();
    Task* const              kParent   = scheduler.current();
    cinux::base::safety::Check(kParent != nullptr, "fork outside any task");
    guard_spawn_depth(*kParent);
    cinux::base::safety::Check(kParent->space.holds(), "fork of a task without a world");
    base::Shared<mm::AddressSpace> space(mm::cow::CloneSharedSpace(*kParent->space));
    if (!space.holds()) {
        return nullptr;
    }
    cinux::base::safety::Check(kParent->vmas.holds(), "fork parent lost its range register");
    base::Shared<mm::VmaStore> vmas(kParent->vmas->clone());
    if (!vmas.holds()) {
        return nullptr;
    }
    const cinux::base::PhysAddr kStack = mm::Pmm::self().allocate_pages(kStackOrder);
    if (kStack == cinux::base::PhysAddr{}) {
        return nullptr;
    }
    base::Own<Task> child = base::MakeOwn<Task>();
    if (!child.holds()) {
        mm::Pmm::self().free_pages(kStack, kStackOrder);
        return nullptr;
    }
    Task* const kBorn              = child.get();
    kBorn->ctx                     = kParent->ctx;
    kBorn->ctx.fpu                 = kParent->entry_fpu;
    kBorn->ctx.r12                 = kUserR12;
    kBorn->ctx.r13                 = kUserR13;
    kBorn->ctx.r14                 = kUserR14;
    kBorn->ctx.r15                 = kUserR15;
    kBorn->ctx.rip                 = reinterpret_cast<unsigned long long>(&ForkChildEntry);
    const unsigned long kStackVirt = mm::DirectMapVirt(kStack.raw);
    kBorn->ctx.rsp                 = kStackVirt + kStackBytes - sizeof(syscall::SyscallFrame);
    cinux::base::CopyBytes(cinux::base::PtrAt<void>(kBorn->ctx.rsp),
                           cinux::base::PtrAt<const void>(frame_address),
                           sizeof(syscall::SyscallFrame));
    kBorn->state      = TaskState::kReady;
    kBorn->name       = kParent->name;
    kBorn->stack_base = kStackVirt;
    kBorn->user_root  = space->root();
    kBorn->space      = std::move(space);
    kBorn->vmas       = std::move(vmas);
    kBorn->parent     = kParent;
    for (unsigned long slot = 0; slot < syscall::kSignalCount; ++slot) {
        kBorn->signals.actions[slot] = kParent->signals.actions[slot];
    }
    kBorn->signals.blocked = kParent->signals.blocked;
    kBorn->pgid            = kParent->pgid;
    kBorn->sid             = kParent->sid;
    cinux::base::CopyBytes(kBorn->cwd, kParent->cwd, fs::kFsPathMax);
    cinux::base::CopyBytes(kBorn->exe, kParent->exe, fs::kFsPathMax);
    kBorn->controlling_tty = kParent->controlling_tty;
    cinux::base::CopyBytes(kBorn->comm, kParent->comm, sizeof(kBorn->comm));
    cinux::base::CopyBytes(kBorn->limits, kParent->limits, sizeof(kBorn->limits));
    for (unsigned long descriptor = 0; descriptor < fs::kFileTableMax; ++descriptor) {
        const base::Result<base::Shared<fs::OpenFile>> kOpen = kParent->files->shared(descriptor);
        if (!kOpen.ok()) {
            continue;
        }
        const base::Result<unsigned long> kSeated =
            kBorn->files->adopt_at(kOpen.value(), descriptor);
        cinux::base::safety::Check(kSeated.ok(), "fork lost a descriptor it meant to inherit");
        cinux::base::safety::Check(
            kBorn->files->set_cloexec(descriptor, kParent->files->cloexec(descriptor)).ok(),
            "fork lost an inherited descriptor flag");
    }
    return child.release();
}

}  // namespace cinux::proc
