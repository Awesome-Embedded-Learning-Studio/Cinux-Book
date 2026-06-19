/**
 * @file kernel/proc/clone.cpp
 * @brief clone() -- Linux-style thread/process creation (F3-M2 batch 4)
 *
 * Creates a new task that shares resources with the caller per the CLONE_*
 * flags (CLONE_VM/FILES/SIGHAND/FS share; their absence => copy semantics like
 * fork).  CLONE_THREAD puts the child in the caller's thread group; CLONE_SETTLS
 * sets the child's FS base; CLONE_*SETTID/CLEARTID manage the tid word used by
 * pthread_join.
 *
 * The child returns to user space at the parent's syscall-return RIP with
 * RAX=0 and (if @p stack != 0) RSP=@p stack.  This is achieved by copying the
 * parent's kernel stack (whose syscall pt_regs frame sits at the very top) and
 * patching the child's user-RSP slot -- see GOTCHA#18.
 *
 * Split out of fork.cpp (F3-M2 batch 5) to keep each file under the 500-line
 * soft limit.
 *
 * Namespace: cinux::proc
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
#include "proc/per_cpu.hpp"

namespace cinux::proc {

namespace {

// FLAG_* / phys_to_virt / PT_ENTRIES live in cinux::arch.
using namespace cinux::arch;

// Linux clone() flags we honour.
constexpr uint64_t kCloneVm            = 0x00000100;
constexpr uint64_t kCloneFs            = 0x00000200;
constexpr uint64_t kCloneFiles         = 0x00000400;
constexpr uint64_t kCloneSighand       = 0x00000800;
constexpr uint64_t kCloneThread        = 0x00010000;
constexpr uint64_t kCloneSettls        = 0x00080000;
constexpr uint64_t kCloneParentSettid  = 0x00100000;
constexpr uint64_t kCloneChildCleartid = 0x00200000;
constexpr uint64_t kCloneChildSettid   = 0x01000000;

// Size of the syscall_entry pt_regs frame (12 qwords): user_rsp..rbp.  It sits
// at the very top of the kernel stack ([kernel_stack_top-96, kernel_stack_top))
// because syscall_entry loads RSP from %gs:0 == kernel_stack_top.  clone uses
// this to patch the child's user-RSP slot.
constexpr uint64_t kSyscallFrameSize = 96;

/**
 * @brief Copy the parent's CoW page tables + VMA records into @p child.
 *
 * Used for the non-CLONE_VM case (clone without CLONE_VM == fork-like CoW).
 * Calls copy_page_table_level() shared with fork.cpp via process_internal.
 */
void cow_clone_address_space(Task* parent, Task* child) {
    if (parent->addr_space == nullptr) {
        return;
    }
    child->addr_space = new cinux::mm::AddressSpace();
    if (child->addr_space == nullptr) {
        cinux::lib::kprintf("[PROC] clone: child address space allocation failed\n");
        return;
    }

    uint64_t parent_pml4 = parent->addr_space->pml4_phys();
    uint64_t child_pml4  = child->addr_space->pml4_phys();

    auto* parent_pml4_table = phys_to_virt(parent_pml4);
    auto* child_pml4_table  = phys_to_virt(child_pml4);

    for (uint32_t i = 0; i < 256; i++) {
        if (!parent_pml4_table[i].is_present()) {
            continue;
        }
        if (!(parent_pml4_table[i].raw & FLAG_USER)) {
            continue;
        }
        uint64_t new_page = cinux::mm::g_pmm.alloc_page();
        if (new_page == 0) {
            cinux::lib::kprintf("[PROC] clone: page table allocation failed at PML4[%u]\n", i);
            break;
        }
        auto* new_table = phys_to_virt(new_page);
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            new_table[j].raw = 0;
        }
        child_pml4_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
        copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
    }

    // Clone VMA records so bookkeeping matches the CoW page tables.
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

}  // anonymous namespace

__attribute__((optimize("no-omit-frame-pointer"), noinline)) int clone(
    uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls) {
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[PROC] clone: no current task\n");
        return -1;
    }
    // CLONE_SIGHAND without CLONE_VM is invalid (can't share dispositions
    // without sharing the address space).
    if ((flags & kCloneSighand) && !(flags & kCloneVm)) {
        return -22;  // EINVAL
    }

    int child_pid = cinux::proc::g_pid_alloc.alloc();
    if (child_pid == PidAllocator::PID_NONE) {
        cinux::lib::kprintf("[PROC] clone: PID allocator exhausted\n");
        return -12;  // ENOMEM
    }

    auto* child = new (std::align_val_t{alignof(Task)}) Task;
    if (child == nullptr) {
        cinux::proc::g_pid_alloc.free(child_pid);
        return -12;
    }
    std::memcpy(child, parent, sizeof(Task));

    // ---- Shared resources: share (acquire) or copy, per flags ----
    // Detach the pointers inherited from the memcpy FIRST; any error-path
    // `delete child` below then releases only the child's own objects.
    if ((flags & kCloneSighand) && parent->sig_actions != nullptr) {
        child->sig_actions = parent->sig_actions;
        child->sig_actions->acquire();
    } else {
        child->sig_actions = SharedSigActions::create_copy(parent->sig_actions);
    }
    if ((flags & kCloneFs) && parent->cwd != nullptr) {
        child->cwd = parent->cwd;
        child->cwd->acquire();
    } else {
        child->cwd = SharedCwd::create_copy(parent->cwd);
    }
    child->fd_table    = nullptr;  // detached; rebuilt/shared below
    child->sig_pending = 0;
    if (child->sig_actions == nullptr || child->cwd == nullptr) {
        cinux::lib::kprintf("[PROC] clone: shared-state alloc failed\n");
        delete child;
        cinux::proc::g_pid_alloc.free(child_pid);
        return -12;
    }

    child->tid             = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    child->pid             = child_pid;
    child->clear_child_tid = (flags & kCloneChildCleartid) ? child_tid : 0;
    child->set_child_tid   = (flags & kCloneChildSettid) ? child_tid : 0;

    // ---- Thread group identity ----
    if (flags & kCloneThread) {
        // Sibling: shares the caller's group and parent.
        child->tgid         = parent->tgid;
        child->group_leader = parent->group_leader;
        child->ppid         = parent->ppid;
        child->parent       = parent->parent;
    } else {
        // New process: own group, caller is the parent.
        child->tgid         = child_pid;
        child->group_leader = child;
        child->ppid         = parent->pid;
        child->parent       = parent;
    }
    child->state       = TaskState::Ready;
    child->children    = nullptr;
    child->exit_status = 0;

    // F3-M3 batch 1: derive process-group / session membership (threads and
    // forked processes alike stay in the caller's group/session; a root fork
    // founds a new one).  See inherit_process_identity.
    inherit_process_identity(child, parent, child_pid);

    // ---- Kernel stack (always per-thread): copy parent's used portion ----
    uint64_t child_stack_phys = cinux::mm::g_pmm.alloc_pages(TaskBuilder::STACK_PAGES);
    if (child_stack_phys == 0) {
        cinux::lib::kprintf("[PROC] clone: child stack allocation failed\n");
        delete child;
        cinux::proc::g_pid_alloc.free(child_pid);
        return -12;
    }
    uint64_t child_guard_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES + 1);
    uint64_t child_stack_virt = child_guard_virt + cinux::arch::PAGE_SIZE;
    uint64_t stack_size       = TaskBuilder::STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < TaskBuilder::STACK_PAGES; i++) {
        uint64_t phys = child_stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = child_stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] clone: child stack map failed at page %u\n",
                                static_cast<unsigned>(i));
            delete child;
            cinux::proc::g_pid_alloc.free(child_pid);
            return -12;
        }
    }
    *reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;

    child->kernel_stack            = child_stack_virt;
    child->kernel_stack_top        = child_stack_virt + stack_size;
    child->kernel_stack_guard_page = child_guard_virt;

    uint64_t current_rsp;
    uint64_t current_rbp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
    __asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

    uint64_t full_stack_used = parent->kernel_stack_top - current_rsp;
    // Defensive: cap the copied region at the child stack size so a
    // pathologically-deep parent (or a synthetic test parent) cannot make
    // child_stack_start underflow below the stack mapping.
    if (full_stack_used > stack_size) {
        full_stack_used = stack_size;
    }
    uint64_t child_stack_start = child_stack_virt + stack_size - full_stack_used;
    std::memcpy(reinterpret_cast<void*>(child_stack_start), reinterpret_cast<void*>(current_rsp),
                full_stack_used);

    // The child resumes via the trampoline (sets rax=0) and unwinds the copied
    // stack frames back out through syscall_entry, exactly like fork.
    child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;
    child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);
    child->ctx.rip = reinterpret_cast<uint64_t>(fork_child_trampoline);

    // ---- CRUX: patch the child's user-RSP to the provided thread stack ----
    // The syscall pt_regs frame is at [kernel_stack_top-96, kernel_stack_top);
    // its first slot (offset 0) is user_rsp.  Overwrite it so the child returns
    // to user space on the caller-supplied stack instead of the parent's.
    if (stack != 0) {
        *reinterpret_cast<uint64_t*>(child->kernel_stack_top - kSyscallFrameSize) = stack;
    }

    // ---- CLONE_SETTLS: child FS base (TLS) ----
    if (flags & kCloneSettls) {
        child->ctx.fs_base = tls;
    }

    // ---- Address space: CLONE_VM shares, else CoW copy ----
    if (flags & kCloneVm) {
        child->addr_space = parent->addr_space;  // shared (no refcount yet)
    } else {
        cow_clone_address_space(parent, child);
    }

    // ---- fd table: CLONE_FILES shares, else copy ----
    if (flags & kCloneFiles) {
        // Share the parent's table (may be nullptr = global); acquire if real.
        child->fd_table = parent->fd_table;
        if (child->fd_table != nullptr) {
            child->fd_table->acquire();
        }
    } else {
        auto* src =
            parent->fd_table != nullptr ? parent->fd_table : &cinux::fs::g_global_fd_table();
        bool has_entries = false;
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

    // ---- Linkage: CLONE_THREAD is a sibling, NOT a child of the caller ----
    if (!(flags & kCloneThread)) {
        child->wait_next = parent->children;
        parent->children = child;
    }

    child->ctx.gs_base  = 0;
    child->ctx.kgs_base = g_per_cpu.gs_page_vaddr;

    // ---- TID flags ----
    if ((flags & kCloneParentSettid) && parent_tid != 0) {
        *reinterpret_cast<int*>(parent_tid) = child_pid;
    }
    if ((flags & kCloneChildSettid) && child_tid != 0) {
        // CLONE_VM shares memory, so writing here is visible to the child.
        *reinterpret_cast<int*>(child_tid) = child_pid;
    }

    Scheduler::add_task(child);

    cinux::lib::kprintf("[PROC] clone: created child pid=%d tid=%lu tgid=%d flags=0x%lx\n",
                        child->pid, child->tid, child->tgid, flags);

    return child_pid;
}

}  // namespace cinux::proc
