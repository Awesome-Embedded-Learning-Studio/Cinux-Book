#include "kernel/proc/clone.hpp"

#include "cinux/assert.hpp"
#include "cinux/ptr.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/proc/fork.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::proc {

Task* BuildCloneChild(unsigned long long frame_address, const ClonePlan& plan) {
    const arch::IrqGuard kEntryGuard;
    const Task* const    kParent = Scheduler::self().current();
    cinux::base::safety::Check(kParent != nullptr, "clone outside any task");
    cinux::base::safety::Check(kParent->space.holds(), "clone of a task without a world");
    cinux::base::safety::Check(kParent->vmas.holds(), "clone parent lost its range register");
    Task* const kBorn = BuildForkChild(frame_address);
    if (kBorn == nullptr) {
        return nullptr;
    }
    if ((plan.flags & syscall::kCloneVm) != 0) {
        kBorn->space     = kParent->space;
        kBorn->vmas      = kParent->vmas;
        kBorn->user_root = kParent->user_root;
    }
    if ((plan.flags & syscall::kCloneFiles) != 0) {
        kBorn->files = kParent->files;
    }
    if ((plan.flags & syscall::kCloneSetTls) != 0) {
        kBorn->ctx.fs_base = plan.tls;
    }
    auto& child_frame    = *cinux::base::PtrAt<syscall::SyscallFrame>(kBorn->ctx.rsp);
    child_frame.user_rsp = plan.user_stack;
    return kBorn;
}

}  // namespace cinux::proc
