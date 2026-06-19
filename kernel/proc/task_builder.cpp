/**
 * @file kernel/proc/task_builder.cpp
 * @brief TaskBuilder setter and build() implementations
 *
 * Provides the fluent setter methods and the build() factory that
 * allocates a TCB, maps a guarded kernel stack, initialises the CPU
 * context, and returns a ready-to-run Task.
 */

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"
#include "proc/per_cpu.hpp"

namespace cinux::proc {

// ============================================================
// TaskBuilder setter implementations
// ============================================================

TaskBuilder& TaskBuilder::set_entry(void (*entry)()) {
    entry_ = entry;
    return *this;
}

TaskBuilder& TaskBuilder::set_name(const char* name) {
    name_ = name;
    return *this;
}

TaskBuilder& TaskBuilder::set_priority(uint64_t priority) {
    priority_ = priority;
    return *this;
}

TaskBuilder& TaskBuilder::set_addr_space(cinux::mm::AddressSpace* space) {
    addr_space_ = space;
    return *this;
}

TaskBuilder& TaskBuilder::set_sched_class(SchedulingClass* sched_class) {
    sched_class_ = sched_class;
    return *this;
}

// ============================================================
// TaskBuilder::build
// ============================================================

Task* TaskBuilder::build() {
    if (entry_ == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: entry point is null\n");
        return nullptr;
    }

    auto* task = new (std::align_val_t{alignof(Task)}) Task;
    if (task == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: TCB allocation failed\n");
        return nullptr;
    }

    for (uint8_t* p = reinterpret_cast<uint8_t*>(task); p < reinterpret_cast<uint8_t*>(task + 1);
         p++) {
        *p = 0;
    }

    uint64_t stack_phys = cinux::mm::g_pmm.alloc_pages(STACK_PAGES);
    if (stack_phys == 0) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: stack allocation failed\n");
        delete task;
        return nullptr;
    }

    uint64_t guard_virt = alloc_stack_vaddr(STACK_PAGES + 1);
    uint64_t stack_virt = guard_virt + cinux::arch::PAGE_SIZE;
    uint64_t stack_size = STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < STACK_PAGES; i++) {
        uint64_t phys = stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] TaskBuilder::build: stack map failed at page %lu\n", i);
            delete task;
            return nullptr;
        }
    }

    *reinterpret_cast<uint64_t*>(stack_virt) = STACK_MAGIC;

    task->ctx.rsp = stack_virt + stack_size - 8;
    *reinterpret_cast<uint64_t*>(task->ctx.rsp) =
        reinterpret_cast<uint64_t>(&cinux::proc::Scheduler::exit_current);
    task->ctx.rip      = reinterpret_cast<uint64_t>(entry_);
    task->ctx.r15      = 0;
    task->ctx.r14      = 0;
    task->ctx.r13      = 0;
    task->ctx.r12      = 0;
    task->ctx.rbp      = 0;
    task->ctx.rbx      = 0;
    task->ctx.gs_base  = 0;
    task->ctx.kgs_base = g_per_cpu.gs_page_vaddr;
    task->ctx.fs_base  = 0;  // F3-M2: no TLS until clone(CLONE_SETTLS)

    __asm__ volatile("fninit");
    __asm__ volatile("fxsave %0" : : "m"(task->fpu_state));

    task->state                   = TaskState::Ready;
    task->tid                     = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    task->priority                = priority_;
    task->kernel_stack            = stack_virt;
    task->kernel_stack_top        = stack_virt + stack_size;
    task->kernel_stack_guard_page = guard_virt;
    task->addr_space              = addr_space_;
    task->sched_class             = sched_class_;
    task->name                    = name_;

    // F3-M2 batch 4: kernel threads are their own (trivial) thread group.
    task->pid             = 0;  // kernel threads have no PidAllocator id
    task->tgid            = 0;
    task->group_leader    = task;
    task->clear_child_tid = 0;
    task->set_child_tid   = 0;

    // F3-M2 batch 3: fresh tasks own their own refcounted shared resources.
    // (The stack-map error path above runs before this point and leaves these
    // nullptr, which release_resources handles safely.)
    task->sig_actions = SharedSigActions::create();
    task->cwd         = SharedCwd::create();
    if (task->sig_actions == nullptr || task->cwd == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: shared-state alloc failed\n");
        delete task;
        return nullptr;
    }

    cinux::lib::kprintf("[PROC] Created task tid=%lu name='%s' stack=0x%p\n", task->tid, task->name,
                        reinterpret_cast<void*>(task->kernel_stack_top));

    return task;
}

// ============================================================
// Shared-resource teardown (F3-M2 batch 3)
// ============================================================

void Task::release_resources() {
    // Drop this task's references to its refcounted shared objects.  Each is
    // either a private copy (refcount 1 -> freed) or a shared object (refcount
    // merely decremented).  fd_table is forward-declared in process.hpp, so the
    // call to its release() lives here (file.hpp is fully included).
    if (sig_actions != nullptr) {
        sig_actions->release();
        sig_actions = nullptr;
    }
    if (cwd != nullptr) {
        cwd->release();
        cwd = nullptr;
    }
    if (fd_table != nullptr) {
        fd_table->release();
        fd_table = nullptr;
    }
}

}  // namespace cinux::proc
