#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/halt.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

namespace {

/// log2(kStackPages); the PMM frees with the same order it granted.
constexpr int kStackOrder = 2;

static_assert((1U << kStackOrder) == kStackPages, "order matches the stack size");

[[noreturn]] void task_exit_hook() {
    Scheduler::self().exit_current();
    cinux::arch::Halt();
}

class KernelSwitchSink {
public:
    void switch_tasks(Task& outgoing, Task& incoming) {
        ContextSwitch(&outgoing.ctx, &incoming.ctx);
    }

    void enter_from_main(Task& incoming) { ContextSwitch(&main_ctx_, &incoming.ctx); }

    void return_to_main(Task& outgoing) { ContextSwitch(&outgoing.ctx, &main_ctx_); }

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

Task* TaskBuilder::build() {
    if (entry_ == nullptr) {
        return nullptr;
    }
    const cinux::base::PhysAddr kBase = cinux::mm::Pmm::self().allocate_pages(kStackOrder);
    if (kBase == cinux::base::PhysAddr{}) {
        return nullptr;
    }
    auto* const kTask = new Task{};
    if (kTask == nullptr) {
        cinux::mm::Pmm::self().free_pages(kBase, kStackOrder);
        return nullptr;
    }
    const unsigned long kStackVirt = cinux::mm::DirectMapVirt(kBase.raw);
    const StartPlan     kPlan{.trampoline   = reinterpret_cast<unsigned long long>(&TaskTrampoline),
                              .entry        = reinterpret_cast<unsigned long long>(entry_),
                              .exit_hook    = reinterpret_cast<unsigned long long>(&task_exit_hook),
                              .stack_bottom = kStackVirt,
                              .stack_top    = kStackVirt + kStackBytes};
    PrepareTaskContext(kTask->ctx, kPlan);
    kTask->state      = TaskState::kReady;
    kTask->name       = name_;
    kTask->stack_base = kStackVirt;
    return kTask;
}

}  // namespace cinux::proc
