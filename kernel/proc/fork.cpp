#include "kernel/proc/fork.hpp"

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/own.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
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

Task* BuildForkChild(unsigned long long frame_address) {
    const arch::IrqGuard     kEntryGuard;
    const auto&              bank      = arch::per_cpu::PerCpu::self();
    const unsigned long long kUserR12  = bank.user_r12;
    const unsigned long long kUserR13  = bank.user_r13;
    const unsigned long long kUserR14  = bank.user_r14;
    const unsigned long long kUserR15  = bank.user_r15;
    const auto&              scheduler = Scheduler::self();
    Task* const              kParent   = scheduler.current();
    cinux::base::safety::Check(kParent != nullptr && kParent->space != nullptr,
                               "fork of a task without a world");
    base::Own<mm::AddressSpace> space(mm::cow::CloneSharedSpace(*kParent->space));
    if (!space.holds()) {
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
    Task* const kBorn = child.get();
    kBorn->ctx        = kParent->ctx;
    asm volatile("fxsave64 %0" : "=m"(kBorn->ctx.fpu) : : "memory");
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
    kBorn->space      = space.release();
    kBorn->parent     = kParent;
    cinux::base::CopyBytes(kBorn->cwd, kParent->cwd, fs::kFsPathMax);
    for (unsigned long descriptor = fs::kFirstDescriptor; descriptor < fs::kFileTableMax;
         ++descriptor) {
        const base::Result<fs::FileSlot*> kSlot = kParent->files.get(descriptor);
        if (!kSlot.ok()) {
            continue;
        }
        const fs::FileSlot&               source = *kSlot.value();
        const base::Result<unsigned long> kOpened =
            kBorn->files.alloc(source.ops, source.backend, source.node);
        if (kOpened.ok()) {
            const base::Result<fs::FileSlot*> kFresh = kBorn->files.get(kOpened.value());
            cinux::base::safety::Check(kFresh.ok(), "fork lost the descriptor it just opened");
            kFresh.value()->offset = source.offset;
        }
    }
    return child.release();
}

}  // namespace cinux::proc
