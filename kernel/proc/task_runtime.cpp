#include "cinux/own.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

namespace {

static_assert((1U << static_cast<unsigned>(kStackOrder)) == kStackPages,
              "order matches the stack size");

[[noreturn]] void task_exit_hook() {
    Scheduler::self().exit_current(0);
    cinux::arch::Halt();
}

void arm_task_entry(const Task& task) {
    unsigned long long const kStackTop              = task.stack_base + kStackBytes;
    cinux::arch::tss::Tss::self().rsp0              = kStackTop;
    cinux::arch::per_cpu::PerCpu::self().kernel_rsp = kStackTop;
    unsigned long long const kRoot =
        (task.user_root != 0) ? task.user_root : cinux::mm::KernelPageRoot();
    if (cinux::arch::ReadCr3() != kRoot) {
        cinux::arch::LoadCr3(kRoot);
    }
}

void swap_address_regs(Task* outgoing, const Task* incoming) {
    if (outgoing != nullptr) {
        cinux::arch::per_cpu::StoreTaskAddressRegs(outgoing->ctx.fs_base, outgoing->ctx.gs_base);
    }
    if (incoming != nullptr) {
        cinux::arch::per_cpu::LoadTaskAddressRegs(incoming->ctx.fs_base, incoming->ctx.gs_base);
    }
}

class KernelSwitchSink {
public:
    void switch_tasks(Task& outgoing, Task& incoming) {
        arm_task_entry(incoming);
        swap_address_regs(&outgoing, &incoming);
        ContextSwitch(&outgoing.ctx, &incoming.ctx);
    }

    void enter_from_main(Task& incoming) {
        arm_task_entry(incoming);
        swap_address_regs(nullptr, &incoming);
        ContextSwitch(&main_ctx_, &incoming.ctx);
    }

    void return_to_main(Task& outgoing) {
        if (cinux::arch::ReadCr3() != cinux::mm::KernelPageRoot()) {
            cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
        }
        swap_address_regs(&outgoing, nullptr);
        ContextSwitch(&outgoing.ctx, &main_ctx_);
    }

    void reclaim(Task& dead) {
        const auto kStackPhys = cinux::mm::DirectMapPhys(dead.stack_base);
        cinux::mm::Pmm::self().free_pages(cinux::base::PhysAddr{kStackPhys}, kStackOrder);
        delete &dead;
    }

private:
    cinux::arch::CpuContext main_ctx_{};
};

KernelSwitchSink g_kernel_sink;

}  // namespace

void InstallKernelSwitchSink() {
    Scheduler::self().init(g_kernel_sink);
}

TaskBuilder& TaskBuilder::set_entry(void (*entry)()) {
    entry_ = entry;
    return *this;
}

TaskBuilder& TaskBuilder::set_name(const char* name) {
    name_ = name;
    return *this;
}

TaskBuilder& TaskBuilder::set_parent(Task& parent) {
    parent_ = &parent;
    return *this;
}

TaskBuilder& TaskBuilder::set_priority(unsigned char priority) {
    priority_ = priority;
    return *this;
}

TaskBuilder& TaskBuilder::set_class(SchedulingClass& cls) {
    class_ = &cls;
    return *this;
}

Task* TaskBuilder::build() {
    if (entry_ == nullptr) {
        return nullptr;
    }
    const cinux::base::PhysAddr kBase = cinux::mm::Pmm::self().allocate_pages(kStackOrder);
    if (kBase == cinux::base::PhysAddr{}) {
        return nullptr;
    }
    base::Own<Task> k_task = base::MakeOwn<Task>();
    if (!k_task.holds()) {
        cinux::mm::Pmm::self().free_pages(kBase, kStackOrder);
        return nullptr;
    }
    Task* const         kBorn      = k_task.get();
    const unsigned long kStackVirt = cinux::mm::DirectMapVirt(kBase.raw);
    const StartPlan     kPlan{
        .trampoline   = reinterpret_cast<unsigned long long>(&TaskTrampoline),
        .entry        = reinterpret_cast<unsigned long long>(entry_),
        .exit_hook    = reinterpret_cast<unsigned long long>(&task_exit_hook),
        .stack_bottom = kStackVirt,
        .stack_top    = kStackVirt + kStackBytes,
    };
    PrepareTaskContext(kBorn->ctx, kPlan);
    kBorn->state       = TaskState::kReady;
    kBorn->name        = name_;
    kBorn->stack_base  = kStackVirt;
    kBorn->parent      = parent_;
    kBorn->priority    = priority_;
    kBorn->sched_class = class_;
    return k_task.release();
}

}  // namespace cinux::proc
