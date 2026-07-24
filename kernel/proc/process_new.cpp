/**
 * @file kernel/proc/process.cpp
 * @brief Shared process-internal state, CoW fault handler, and waitpid
 *
 * Houses the TID counter and stack-virtual-address allocator used by
 * TaskBuilder::build() and fork(), the Copy-On-Write page fault
 * resolver, and the waitpid() system call implementation.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/tlb.hpp"  // B3 defect C: enqueue_pending_shootdown
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"  // Q4e-2: free_kernel_stack translate/unmap
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // Q4e-2: signal_unregister_task on reap

namespace cinux::proc {

// ============================================================
// Shared internal state (used by task_builder.cpp and fork.cpp)
// ============================================================

cinux::lib::Atomic<uint64_t> next_tid{1};

cinux::lib::Atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};

uint64_t alloc_stack_vaddr(uint64_t pages) {
    uint64_t vaddr = next_stack_vaddr.fetch_add(pages * cinux::arch::PAGE_SIZE,
                                                cinux::lib::MemoryOrder::Relaxed);
    return vaddr;
}

// ============================================================
// CoW page fault handler
// ============================================================

namespace {

using namespace cinux::arch;

PageEntry* get_pte(uint64_t pml4_phys, uint64_t virt) {
    auto*      pml4  = phys_to_virt(pml4_phys);
    PageEntry& pml4e = pml4[PML4_INDEX(virt)];
    if (!pml4e.is_present())
        return nullptr;

    auto*      pdpt  = phys_to_virt(pml4e.phys_addr());
    PageEntry& pdpte = pdpt[PDPT_INDEX(virt)];
    if (!pdpte.is_present())
        return nullptr;

    auto*      pd  = phys_to_virt(pdpte.phys_addr());
    PageEntry& pde = pd[PD_INDEX(virt)];
    if (!pde.is_present())
        return nullptr;

    auto* pt = phys_to_virt(pde.phys_addr());
    return &pt[PT_INDEX(virt)];
}

}  // anonymous namespace

bool handle_cow_fault(uint64_t fault_vaddr) {
    auto* task = Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return false;
    }

    uint64_t   pml4_phys = task->addr_space->pml4_phys();
    PageEntry* pte       = get_pte(pml4_phys, fault_vaddr);
    if (pte == nullptr) {
        return false;
    }

    if (!pte->is_present())
        return false;
    if (pte->raw & FLAG_WRITABLE)
        return false;
    if (!(pte->raw & FLAG_COW))
        return false;

    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();
    if (new_phys == 0) {
        cinux::lib::kprintf("[COW] page allocation failed for vaddr=%p\n",
                            reinterpret_cast<void*>(fault_vaddr));
        return false;
    }

    auto* src = reinterpret_cast<uint8_t*>(old_phys + cinux::arch::DIRECT_MAP_BASE);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + cinux::arch::DIRECT_MAP_BASE);
    for (uint64_t i = 0; i < cinux::arch::PAGE_SIZE; i++) {
        dst[i] = src[i];
    }

    pte->set_phys_addr(new_phys);
    pte->raw |= FLAG_WRITABLE;
    pte->raw &= ~FLAG_COW;
    cinux::mm::g_pmm.pte_count_inc(new_phys);  // batch 3: account for the new private-copy PTE

    cinux::arch::flush_tlb(fault_vaddr & ~(cinux::arch::PAGE_SIZE - 1));

    // Q4b-3 (DEBT-003): this PTE no longer maps old_phys (TLB flushed above).
    // Drop the old page's mapping ref; pte_count_dec_and_test frees it on the
    // last ownership ref (batch 3: was a two-step dec_and_test + free_page).
    // NOTE (SMP): correct single-core and when threads do not migrate across
    // cores mid-CoW. Cross-core TLB shootdown before freeing is a deeper
    // follow-up; Cinux APs are mostly idle today.
    // B3 defect C: defer the free.  pte_count_dec_and_test_no_free returns
    // true when the page would be freed (pte_count+refcount hit 0, audit
    // passed) but does NOT free -- another core's TLB may still cache a
    // mapping to old_phys.  enqueue_pending_shootdown either pushes (phys,
    // vaddr) for the drain kthread to shootdown+free (production), or falls
    // back to an inline free_page (suite-only / pre-init, single-core-safe).
    if (cinux::mm::g_pmm.pte_count_dec_and_test_no_free(old_phys)) {
        cinux::arch::enqueue_pending_shootdown(old_phys, fault_vaddr);
    }

    return true;
}

// ============================================================
// F-QA Q4e-2 (DEBT-002): free a reaped task's kernel stack
// ============================================================

// The stack was mapped (g_vmm.map, NOT direct-map) at [kernel_stack,
// kernel_stack_top) with a guard page below. Recover phys via translate
// (4K pages -- GOTCHA#13: translate cannot handle huge, but stacks are 4K),
// unmap each page, then free the physical block. The guard+stack vaddr range
// is not recycled (the vaddr allocator is linear; address space is large).
//
// Safe ONLY when the task is not running on this stack -- the caller is the
// reaper (waitpid on the parent). exit_current() cannot use this directly
// (it runs on its own stack); that path uses deferred free (Q4e-3).
void free_kernel_stack(Task* task) {
    if (task->kernel_stack == 0) {
        return;
    }
    uint64_t stack_phys = cinux::mm::g_vmm.translate(task->kernel_stack);
    for (uint64_t v = task->kernel_stack; v < task->kernel_stack_top; v += cinux::arch::PAGE_SIZE) {
        cinux::mm::g_vmm.unmap(v);
    }
    if (stack_phys != 0) {
        uint64_t count = (task->kernel_stack_top - task->kernel_stack) / cinux::arch::PAGE_SIZE;
        cinux::mm::g_pmm.free_pages(stack_phys, count);
    }
    task->kernel_stack = 0;
}

// ============================================================
// waitpid implementation
// ============================================================

WaitpidResult waitpid(int pid, int* status, int options, PidAllocator& pid_alloc, int* reaped_pid) {
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[WAITPID] no current task\n");
        return WaitpidResult::NoChildren;
    }

    if (pid != -1 && pid <= 0) {
        cinux::lib::kprintf("[WAITPID] invalid pid=%d\n", pid);
        return WaitpidResult::InvalidPid;
    }

    // Re-scan loop: after waking from a block, the exited child is now Zombie.
    for (;;) {
        if (parent->children == nullptr) {
            return WaitpidResult::NoChildren;
        }

        // Scan the children list for a Zombie matching the pid selector.
        Task* target   = nullptr;
        Task* prev     = nullptr;
        Task* cur      = parent->children;
        Task* cur_prev = nullptr;
        while (cur != nullptr) {
            if ((pid == -1 || cur->pid == pid) && cur->state == TaskState::Zombie) {
                target = cur;
                prev   = cur_prev;
                break;
            }
            cur_prev = cur;
            cur      = cur->wait_next;
        }

        if (target != nullptr) {
            // Reap: collect status, unlink, free pid, mark Dead.
            int target_pid = target->pid;
            if (status != nullptr) {
                *status = target->exit_status;
            }
            if (reaped_pid != nullptr) {
                *reaped_pid = target_pid;
            }
            if (prev != nullptr) {
                prev->wait_next = target->wait_next;
            } else {
                parent->children = target->wait_next;
            }
            pid_alloc.free(target->pid);
            target->state  = TaskState::Dead;
            target->parent = nullptr;
            cinux::lib::kprintf("[WAITPID] reaped child pid=%d exit_status=%d by parent pid=%d\n",
                                target_pid, target->exit_status, parent->pid);
            // SMP reap/free safety: the child exited (became Zombie) on its own
            // CPU BEFORE its yield()->schedule()->context_switch finished. We can
            // observe Zombie here on the parent's CPU while the child's CPU is
            // still mid-context-switch writing the child's ctx / still on its
            // kernel stack. Freeing the stack + struct then is a cross-core UAF
            // (the exit/reap "resurrection" bug: the freed memory is reused while
            // the child's CPU keeps executing on it, producing garbage exits).
            // context_switch.S clears from->on_cpu to -1 once the child's ctx save
            // is complete and it has left the stack; ACQUIRE-load that as the
            // rendezvous. Bounded spin -- the child already yielded to idle, so its
            // switch finishes within microseconds. (Single-core: the child's switch
            // ran to completion before the parent re-entered waitpid, so on_cpu is
            // already -1 and the loop exits immediately.)
            for (int spin = 0; spin < 50'000'000; ++spin) {
                if (__atomic_load_n(&target->on_cpu, __ATOMIC_ACQUIRE) == -1) {
                    break;
                }
                __asm__ volatile("pause");
            }
            if (__atomic_load_n(&target->on_cpu, __ATOMIC_ACQUIRE) != -1) {
                // Should not happen (the child's CPU clears on_cpu on its next
                // schedule). Leak the struct rather than risk a UAF -- a pinned
                // child is debuggable; a use-after-free is not.
                cinux::lib::kprintf(
                    "[WAITPID] WARN: child pid=%d still on_cpu=%d after spin -- "
                    "leaking struct (no free)\n",
                    target_pid, target->on_cpu);
                return WaitpidResult::Ok;
            }
            // Q4e-2 (DEBT-002): now that the child's switch is provably done,
            // freeing its kernel stack + struct is safe. delete -> release_resources
            // drops sig_actions/cwd/fd_table + the AS ref.
            // Q4e-2: detach from the pid registry before freeing (sys_exit left
            // the Zombie registered for sys_kill lookup; reap deletes it).
            // exit_current already unregisters; this mirrors it for the
            // Zombie->reap path so the registry never holds a freed Task*.
            signal_unregister_task(target);
            free_kernel_stack(target);
            delete target;
            return WaitpidResult::Ok;
        }

        // No Zombie yet.  For a specific pid, distinguish "not a child"
        // (NotFound) from "child exists but still running" (NotExited).
        if (pid != -1) {
            cur           = parent->children;
            bool is_child = false;
            while (cur != nullptr) {
                if (cur->pid == pid) {
                    is_child = true;
                    break;
                }
                cur = cur->wait_next;
            }
            if (!is_child) {
                cinux::lib::kprintf("[WAITPID] pid=%d is not a child of pid=%d\n", pid,
                                    parent->pid);
                return WaitpidResult::NotFound;
            }
        }

        // A matching child exists but has not exited.
        if (options & kWaitNoHang) {
            return WaitpidResult::NotExited;
        }

        // Block until a child exits.  sys_exit() unconditionally Scheduler::unblock()s
        // us (F-QA Q4c-1 / DEBT-004: was gated on a non-atomic bool, now unconditional
        // since unblock is idempotent).  F4-M4 prepare-to-wait: mark Blocked
        // before switching so a concurrent sys_exit() racing through the window
        // finds us Blocked and wakes us (unblock() is idempotent) -- closing the
        // old single-core-only "check+block is atomic" assumption.
        //
        // F4-M5 analysis: the children list needs NO lock for SMP, despite the
        // earlier "follow-up" note.  `Task::children` is per-task PRIVATE: fork
        // and clone link a child into current()->children, and CLONE_THREAD is a
        // sibling (NOT a child, clone.cpp), so a thread group does not share one
        // list.  waitpid scans/reaps current()->children, and sys_exit() does NOT
        // touch parent->children (it only reads task->parent to wake it) -- there
        // is no reparenting.  So each list is touched only by its owning task,
        // which runs on one CPU at a time: no cross-CPU list access.  The one
        // cross-CPU datum is child->state (set Zombie by the child's exit on CPU
        // A, read here on CPU B) -- atomic on x86, and a miss is covered by
        // exit's unblock(parent) forcing a re-scan.  Hence no children lock.
        //
        // schedule_blocked() -> schedule() needs a real scheduler loop, so this
        // path runs only on real hardware, not in run-kernel-test (kWaitNoHang).
        Scheduler::prepare_to_wait(parent);
        Scheduler::schedule_blocked();
        // loop back: the exited child is now Zombie and will be reaped above.
    }
}

// ============================================================
// Process group / session inheritance (F3-M3 batch 1)
// ============================================================

void inherit_process_identity(Task* child, const Task* parent, int child_pid) {
    if (parent->pgid == 0) {
        // Root fork: the parent is a kernel/bootstrap task with no group, so
        // the child founds a brand-new process group and session and leads it.
        child->pgid           = child_pid;
        child->sid            = child_pid;
        child->session_leader = child;
    } else {
        // Inherit the parent's group, session, leader, and controlling tty.
        child->pgid           = parent->pgid;
        child->sid            = parent->sid;
        child->session_leader = parent->session_leader;
    }
    child->controlling_tty = parent->controlling_tty;
}

}  // namespace cinux::proc
