/**
 * @file kernel/proc/fork.cpp
 * @brief fork() implementation with Copy-On-Write page table cloning
 *
 * Creates a near-identical copy of the current task: new PID, new TCB,
 * new kernel stack (with parent stack contents copied), and CoW page
 * table setup so that user-space pages are shared read-only until
 * either process writes to them.
 */

#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// Internal helpers for CoW page table cloning
// ============================================================

// FLAG_* / phys_to_virt / PT_ENTRIES live in cinux::arch; both the CoW copier
// and fork()'s page-table walk use them unqualified.
using namespace cinux::arch;

/**
 * @brief Recursively copy a page table level for CoW fork/clone
 *
 * At the PT (leaf) level, shares physical pages and marks writable
 * entries as read-only with FLAG_COW.  At intermediate levels, allocates
 * new page table pages and recurses.  Shared with clone.cpp (declared in
 * process_internal.hpp).
 */
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present())
            continue;

        if (!(src_table[i].raw & FLAG_USER))
            continue;

        if (src_table[i].huge) {
            dst_table[i].raw = src_table[i].raw;
            // Q4b-2 (DEBT-003): huge pages are shared too. Userspace has no
            // huge pages yet (GOTCHA#13 huge-split not done), so inc only the
            // base page and leave the 2MB/1GB tail as a TODO.
            cinux::mm::g_pmm.mapcount_inc(src_table[i].phys_addr());
            continue;
        }

        if (level > 1) {
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                cinux::lib::kprintf("[PROC] fork: intermediate PT alloc failed\n");
                continue;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                new_table[j].raw = 0;
            }

            dst_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
            copy_page_table_level(src_table[i].phys_addr(), new_page, level - 1);
        } else {
            uint64_t entry_flags = src_table[i].raw & FLAG_MASK;

            dst_table[i].raw = src_table[i].raw;
            // Q4b-2 (DEBT-003): both PTEs now point at the same physical page
            // (writable-CoW or read-only-shared). Bump its mapcount so a later
            // clear_user_mappings (exec) does not free it while the other side
            // still maps it (fork+exec UAF).
            cinux::mm::g_pmm.mapcount_inc(src_table[i].phys_addr());

            if (entry_flags & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;
                dst_table[i].raw |= FLAG_COW;

                src_table[i].raw &= ~FLAG_WRITABLE;
                src_table[i].raw |= FLAG_COW;
            }
        }
    }
}

static bool copied_stack_contains(uint64_t value, uint64_t start, uint64_t top) {
    return value >= start && value < top;
}

static uint64_t relocate_copied_stack_addr(uint64_t value, uint64_t parent_stack_start,
                                           uint64_t parent_stack_top, uint64_t child_stack_start) {
    if (!copied_stack_contains(value, parent_stack_start, parent_stack_top)) {
        return value;
    }
    return child_stack_start + (value - parent_stack_start);
}

// ============================================================
// Child resume-context setup (Linux-style fork child)
// ============================================================
//
// Two paths, selected by whether the caller has a user address space:
//
//   A. User fork/clone (parent->addr_space != nullptr) -- the shell
//      command-fork (sys_fork) and user pthreads (sys_clone).  The child
//      gets a CLEAN kernel stack carrying only the parent's 128-byte syscall
//      pt_regs frame at the top, then runs ret_from_fork, which restores the
//      user register state from that frame and SYSRETQs to Ring 3 with rax=0.
//      It never copies the parent's kernel stack and never touches an RBP
//      chain, so it cannot be tripped by a compiler-specific frame layout.
//
//   B. Kernel fork/clone (parent->addr_space == nullptr) -- a kernel/bootstrap
//      thread forking (shell launch, ring-3 smoke).  The child must resume the
//      kernel caller mid-execution, so the parent's used stack is copied and
//      the child resumes at the fork return address via fork_child_trampoline.
//      The frame base comes from __builtin_frame_address(0) (compiler-tracked,
//      robust where a raw `movq %rbp` is not), and NO RBP-chain walk is done:
//      the kernel child runs forward into launch_user_program (which does not
//      return) and never unwinds back up through the copied frames.

void prepare_user_fork_context(Task* child, uint64_t parent_kernel_stack_top) {
    // Copy the parent's syscall pt_regs frame (128 B) to the top of the child's
    // clean kernel stack.  ret_from_fork reads user RIP/RSP/RFLAGS + the
    // syscall-preserved registers out of it and SYSRETQs back to user mode
    // (rax=0).
    std::memcpy(reinterpret_cast<void*>(child->kernel_stack_top - kSyscallFrameSize),
                reinterpret_cast<void*>(parent_kernel_stack_top - kSyscallFrameSize),
                kSyscallFrameSize);
    child->ctx.rsp = child->kernel_stack_top - kSyscallFrameSize;
    child->ctx.rip = reinterpret_cast<uint64_t>(ret_from_fork);
}

void prepare_kernel_fork_context(Task* child, uint64_t parent_stack_start,
                                 uint64_t parent_stack_top, uint64_t child_stack_start,
                                 uint64_t parent_frame_base,
                                 const KernelForkCalleeRegs& caller_regs) {
    // ctx.rsp -> the fork/clone return-address slot (frame_base + 8) in the
    // copied stack; fork_child_trampoline does `xor rax,rax; ret`, so the child
    // pops that slot and continues at the caller's return site, with its own
    // rsp/rbp relocated into the child stack.  ctx.rbp is the saved caller rbp
    // ([frame_base]); only the immediate caller frame needs a valid rbp because
    // the kernel child never returns up through the deeper copied frames.
    child->ctx.rsp =
        relocate_copied_stack_addr(parent_frame_base + sizeof(uint64_t), parent_stack_start,
                                   parent_stack_top, child_stack_start);
    child->ctx.rbp =
        relocate_copied_stack_addr(*reinterpret_cast<uint64_t*>(parent_frame_base),
                                   parent_stack_start, parent_stack_top, child_stack_start);
    child->ctx.r15 = relocate_copied_stack_addr(caller_regs.r15, parent_stack_start,
                                                parent_stack_top, child_stack_start);
    child->ctx.r14 = relocate_copied_stack_addr(caller_regs.r14, parent_stack_start,
                                                parent_stack_top, child_stack_start);
    child->ctx.r13 = relocate_copied_stack_addr(caller_regs.r13, parent_stack_start,
                                                parent_stack_top, child_stack_start);
    child->ctx.r12 = relocate_copied_stack_addr(caller_regs.r12, parent_stack_start,
                                                parent_stack_top, child_stack_start);
    child->ctx.rbx = relocate_copied_stack_addr(caller_regs.rbx, parent_stack_start,
                                                parent_stack_top, child_stack_start);
    child->ctx.rip = reinterpret_cast<uint64_t>(fork_child_trampoline);

    // CRITICAL (gcc-13 -O2 #GP root cause): the kernel caller may save stack
    // POINTERS in its own frame beyond the saved-rbp chain.  musl_hello_smoke_entry
    // under gcc-13 computes argv_base = rbp-0x60 at function entry and stores it
    // in a frame slot; gcc-16 recomputes it inline, which is why only gcc-13 #GPs.
    // The child inherited the parent's copy of that slot UNRELOCATED, so it ends
    // up addressing argv[] through the parent's stack -> overwritten ->
    // str_len(garbage argv[i]) -> #GP.  The deleted RBP-chain walk only fixed the
    // saved-rbp links; this scans the WHOLE copied region and relocates every
    // qword that points back into the parent's (now-copied) stack -- saved rbp,
    // argv_base, and any other spill alike.  Non-stack values (return addresses,
    // small integers, foreign pointers) fall outside the range and are untouched.
    uint64_t copied_bytes = parent_stack_top - parent_stack_start;
    for (uint64_t off = 0; off + sizeof(uint64_t) <= copied_bytes; off += sizeof(uint64_t)) {
        uint64_t* slot = reinterpret_cast<uint64_t*>(child_stack_start + off);
        if (copied_stack_contains(*slot, parent_stack_start, parent_stack_top)) {
            *slot = relocate_copied_stack_addr(*slot, parent_stack_start, parent_stack_top,
                                               child_stack_start);
        }
    }
}

// ============================================================
// fork implementation
// ============================================================

__attribute__((noinline)) int fork(PidAllocator& pid_alloc) {
    KernelForkCalleeRegs caller_regs = capture_kernel_fork_callee_regs();
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[PROC] fork: no current task\n");
        return -1;
    }

    int child_pid = pid_alloc.alloc();
    if (child_pid == PidAllocator::PID_NONE) {
        cinux::lib::kprintf("[PROC] fork: PID allocator exhausted\n");
        return -1;
    }

    auto* child = new (std::align_val_t{alignof(Task)}) Task;
    if (child == nullptr) {
        cinux::lib::kprintf("[PROC] fork: TCB allocation failed\n");
        pid_alloc.free(child_pid);
        return -1;
    }

    std::memcpy(child, parent, sizeof(Task));

    // F3-M2 batch 3: the memcpy just copied the parent's shared-resource
    // POINTERS (sig_actions / cwd / fd_table) into the child.  Give the child
    // its OWN private copies now -- fork is copy semantics (clone in batch 4
    // will share instead).  Doing this before any later error-path
    // `delete child` ensures release() frees the child's own objects, not the
    // parent's.  The block mask (sig_blocked) is retained from the memcpy;
    // pending signals are not inherited (POSIX).
    child->sig_actions = SharedSigActions::create_copy(parent->sig_actions);
    child->cwd         = SharedCwd::create_copy(parent->cwd);
    child->fd_table    = nullptr;  // detached; rebuilt fresh below
    child->sig_pending = 0;
    if (child->sig_actions == nullptr || child->cwd == nullptr) {
        cinux::lib::kprintf("[PROC] fork: shared-state copy failed\n");
        delete child;
        pid_alloc.free(child_pid);
        return -1;
    }

    child->tid             = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    child->pid             = child_pid;
    child->tgid            = child_pid;  // F3-M2: a forked process is its own group leader
    child->group_leader    = child;
    child->clear_child_tid = 0;
    child->set_child_tid   = 0;
    child->ppid            = parent->pid;
    child->state           = TaskState::Ready;
    // F10 SMP-race: memcpy just inherited the parent's on_cpu (= the parent's
    // current CPU).  That trips the F4-followup migration-race guard in
    // pick_next(), which reads on_cpu==X as "CPU X is mid-saving this ctx" and
    // skips the task on every other CPU.  A freshly forked child has never run
    // -- its ctx is fully set up right here -- so it must read as "not running /
    // ctx saved" (-1), exactly like a TaskBuilder-built task.  Without this,
    // under -smp the child is pinned to the parent's CPU until first run and
    // violates the guard's invariant.  See [[smp-migration-context-race]].
    child->on_cpu          = -1;
    child->parent          = parent;
    child->children        = nullptr;
    child->exit_status     = 0;

    // F3-M3 batch 1: derive process-group / session membership.  memcpy
    // already copied the parent's pgid/sid/session_leader, but we re-derive
    // them explicitly so root forks found their own group/session and the
    // rule lives in one testable place (see inherit_process_identity).
    inherit_process_identity(child, parent, child_pid);

    uint64_t child_stack_phys = cinux::mm::g_pmm.alloc_pages(TaskBuilder::STACK_PAGES);
    if (child_stack_phys == 0) {
        cinux::lib::kprintf("[PROC] fork: child stack allocation failed\n");
        delete child;
        pid_alloc.free(child_pid);
        return -1;
    }

    uint64_t child_guard_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES + 1);
    uint64_t child_stack_virt = child_guard_virt + cinux::arch::PAGE_SIZE;
    uint64_t stack_size       = TaskBuilder::STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < TaskBuilder::STACK_PAGES; i++) {
        uint64_t phys = child_stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = child_stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] fork: child stack map failed at page %u\n",
                                static_cast<unsigned>(i));
            delete child;
            pid_alloc.free(child_pid);
            return -1;
        }
    }

    *reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;

    child->kernel_stack            = child_stack_virt;
    child->kernel_stack_top        = child_stack_virt + stack_size;
    child->kernel_stack_guard_page = child_guard_virt;

    // ---- Child resume context (Linux-style fork child) ----
    if (parent->addr_space != nullptr) {
        // Path A -- USER fork (shell command-fork via sys_fork).  The child
        // gets a CLEAN kernel stack carrying only the parent's syscall pt_regs
        // frame at the top, then runs ret_from_fork back to Ring 3 (rax=0).
        // No parent-stack copy, no RBP chain: immune to compiler frame-layout
        // quirks.  fs_base is inherited from the memcpy above (child TLS).
        prepare_user_fork_context(child, parent->kernel_stack_top);
    } else {
        // Path B -- KERNEL fork (a kernel/bootstrap thread forking, e.g. the
        // shell-launch and ring-3 smoke path whose child later installs its own
        // address space and launch_user_program's).  Copy the parent's used
        // stack so the caller's frame survives, and resume at the fork return
        // address via fork_child_trampoline.  The frame base is captured with
        // __builtin_frame_address(0) -- compiler-tracked, robust where a raw
        // `movq %rbp` is not under gcc-13 -O2+ubsan.  The kernel is built
        // globally with -fno-omit-frame-pointer, so the SysV layout
        // (ret addr at [frame+8], saved rbp at [frame]) holds.
        uint64_t current_rsp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));

        uint64_t full_stack_used = parent->kernel_stack_top - current_rsp;
        // Defensive: cap the copied region at the child stack size so a
        // pathologically-deep parent (or a synthetic test parent) cannot make
        // child_stack_start underflow below the stack mapping.
        if (full_stack_used > stack_size) {
            full_stack_used = stack_size;
        }
        uint64_t child_stack_start = child_stack_virt + stack_size - full_stack_used;
        std::memcpy(reinterpret_cast<void*>(child_stack_start),
                    reinterpret_cast<void*>(current_rsp), full_stack_used);

        uint64_t frame_base = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
        prepare_kernel_fork_context(child, current_rsp, current_rsp + full_stack_used,
                                    child_stack_start, frame_base, caller_regs);
    }

    // CoW page table handling
    if (parent->addr_space != nullptr) {
        child->addr_space = new cinux::mm::AddressSpace();
        if (child->addr_space == nullptr) {
            cinux::lib::kprintf("[PROC] fork: child address space allocation failed\n");
            delete child;
            pid_alloc.free(child_pid);
            return -1;
        }

        uint64_t parent_pml4 = parent->addr_space->pml4_phys();
        uint64_t child_pml4  = child->addr_space->pml4_phys();

        auto* parent_pml4_table = phys_to_virt(parent_pml4);
        auto* child_pml4_table  = phys_to_virt(child_pml4);

        for (uint32_t i = 0; i < 256; i++) {
            if (!parent_pml4_table[i].is_present())
                continue;

            if (!(parent_pml4_table[i].raw & FLAG_USER))
                continue;

            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                cinux::lib::kprintf("[PROC] fork: page table allocation failed at PML4[%u]\n", i);
                delete child->addr_space;
                delete child;
                pid_alloc.free(child_pid);
                return -1;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                new_table[j].raw = 0;
            }

            child_pml4_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
            copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
        }

        // F10 SMP fix: copy_page_table_level() just cleared FLAG_WRITABLE and set
        // FLAG_COW on the PARENT's own live PTEs -- this CPU still has the parent's
        // cr3 loaded, and fork() runs with IF=0 (SFMASK clears IF on syscall entry,
        // no sti before dispatch) so the parent cannot migrate or be preempted. Until
        // we invalidate the TLB the parent CPU keeps the stale WRITABLE entries and
        // its next user-mode write goes THROUGH to the now-shared physical page,
        // silently corrupting the child's later CoW copy. That is the SMP fork race;
        // single-CPU was stable only because the next context switch reloads cr3
        // (flushing the TLB) before the parent writes again. A LOCAL flush suffices
        // here: only this CPU holds the parent's cr3, and the child's distinct cr3
        // has never been loaded (clean TLB on first run). A cross-CPU shootdown
        // (needed for CLONE_VM threads sharing one address space) is a separate
        // follow-up, not required for the fork path. See note + [[f10-shell-launch-smp-fork-race]].
        flush_tlb_all();

        // F2-M2 batch 4: clone the parent's VMA records into the child so the
        // bookkeeping matches the CoW page tables.  File backings (Inode*) are
        // shared by pointer; their contents are demand-read in M4 (Page Cache).
        for (cinux::mm::VMA* v = parent->addr_space->vmas().first(); v != nullptr;
             v                 = parent->addr_space->vmas().next(v)) {
            (void)child->addr_space->vmas().insert(v->start, v->end, v->flags);
            if (v->backing != nullptr) {
                cinux::mm::VMA* cv = child->addr_space->vmas().find(v->start);
                if (cv != nullptr) {
                    cv->backing     = v->backing;
                    cv->file_offset = v->file_offset;
                }
            }
        }
    }

    child->wait_next = parent->children;
    parent->children = child;

    // Copy FDTable from parent
    {
        auto* src         = parent->fd_table ? parent->fd_table : &cinux::fs::g_global_fd_table();
        bool  has_entries = false;
        for (uint32_t i = 0; i < cinux::fs::FD_TABLE_SIZE; i++) {
            if (src->get(static_cast<int>(i)) != nullptr) {
                has_entries = true;
                break;
            }
        }
        if (has_entries) {
            child->fd_table = new cinux::fs::FDTable();
            for (uint32_t i = 0; i < cinux::fs::FD_TABLE_SIZE; i++) {
                auto* f = src->get(static_cast<int>(i));
                if (f != nullptr) {
                    auto* copy = new cinux::fs::File(f->inode, f->offset, f->flags);
                    child->fd_table->set(static_cast<int>(i), copy);
                }
            }
        }
    }

    Scheduler::add_task(child);

    cinux::lib::kprintf("[PROC] fork: created child pid=%d tid=%lu parent_pid=%d\n", child->pid,
                        child->tid, parent->pid);

    return child_pid;
}

}  // namespace cinux::proc
