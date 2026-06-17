/**
 * @file kernel/arch/x86_64/exception_handlers.cpp
 * @brief CPU exception handler implementations for the big kernel
 *
 * Provides C handler functions for all configured CPU exceptions (vectors 0-14).
 * Called by ISR stubs in interrupts.S with an InterruptFrame* argument.
 *
 * Policy:
 *   - Non-fatal exceptions (#BP, #DB): print info and continue via IRETQ
 *   - Fatal exceptions (all others): print register dump, then cli;hlt forever
 *
 * Exception-type messages go through klog (klog_error/klog_warn) so they
 * enter the dmesg ring buffer with a level, while still printing in real
 * time.  The verbose register dump and kpanic stay on kprintf (real-time
 * diagnostics; the dump is large and kpanic is the halt path).
 */

#include <stdint.h>

#include "arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

extern "C" char __kernel_stack_top[];
extern "C" char __boot_guard_start[];
extern "C" char __boot_guard_end[];

namespace {

using cinux::arch::InterruptFrame;
using cinux::lib::kprintf;
using cinux::mm::g_vmm;

void dump_registers(const InterruptFrame* frame, const char* name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", name, vector);
    kprintf("  RIP   = %p   CS  = 0x%04x\n", reinterpret_cast<void*>(frame->rip),
            static_cast<unsigned>(frame->cs));
    kprintf("  RFLAGS= %p\n", reinterpret_cast<void*>(frame->rflags));
    kprintf("  RSP   = %p   SS  = 0x%04x\n", reinterpret_cast<void*>(frame->rsp),
            static_cast<unsigned>(frame->ss));
    kprintf("  RAX=%p  RBX=%p\n", reinterpret_cast<void*>(frame->rax),
            reinterpret_cast<void*>(frame->rbx));
    kprintf("  RCX=%p  RDX=%p\n", reinterpret_cast<void*>(frame->rcx),
            reinterpret_cast<void*>(frame->rdx));
    kprintf("  RSI=%p  RDI=%p\n", reinterpret_cast<void*>(frame->rsi),
            reinterpret_cast<void*>(frame->rdi));
    kprintf("  RBP=%p  R8 =%p\n", reinterpret_cast<void*>(frame->rbp),
            reinterpret_cast<void*>(frame->r8));
    kprintf("  R9 =%p  R10=%p\n", reinterpret_cast<void*>(frame->r9),
            reinterpret_cast<void*>(frame->r10));
    kprintf("  R11=%p  R12=%p\n", reinterpret_cast<void*>(frame->r11),
            reinterpret_cast<void*>(frame->r12));
    kprintf("  R13=%p  R14=%p\n", reinterpret_cast<void*>(frame->r13),
            reinterpret_cast<void*>(frame->r14));
    kprintf("  R15=%p\n", reinterpret_cast<void*>(frame->r15));
    kprintf("  ERROR CODE = %p\n", reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}

[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // anonymous namespace

// ============================================================
// Exception handlers (C-linkage, called from ISR stubs)
// ============================================================

extern "C" {

void handle_db(InterruptFrame* frame) {
    dump_registers(frame, "#DB", 1);
    klog_warn("Debug exception (#DB), continuing");
}

void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    klog_warn("Breakpoint (#BP) at RIP=%p", reinterpret_cast<void*>(frame->rip));
    klog_warn("Continuing");
}

void handle_de(InterruptFrame* frame) {
    dump_registers(frame, "#DE", 0);
    klog_error("Divide Error (#DE) -- halting");
    fatal_halt();
}

void handle_nmi(InterruptFrame* frame) {
    dump_registers(frame, "NMI", 2);
    klog_error("Non-maskable Interrupt (NMI) -- halting");
    fatal_halt();
}

void handle_of(InterruptFrame* frame) {
    dump_registers(frame, "#OF", 4);
    klog_error("Overflow (#OF) -- halting");
    fatal_halt();
}

void handle_br(InterruptFrame* frame) {
    dump_registers(frame, "#BR", 5);
    klog_error("BOUND Range Exceeded (#BR) -- halting");
    fatal_halt();
}

void handle_ud(InterruptFrame* frame) {
    dump_registers(frame, "#UD", 6);
    klog_error("Invalid Opcode (#UD) -- halting");
    fatal_halt();
}

void handle_nm(InterruptFrame* frame) {
    dump_registers(frame, "#NM", 7);
    klog_error("Device Not Available (#NM) -- halting");
    fatal_halt();
}

void handle_df(InterruptFrame* frame) {
    dump_registers(frame, "#DF", 8);
    klog_error("Double Fault (#DF, error code=%p) -- halting",
               reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_ts(InterruptFrame* frame) {
    dump_registers(frame, "#TS", 10);
    klog_error("Invalid TSS (#TS, error code=%p) -- halting",
               reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_np(InterruptFrame* frame) {
    dump_registers(frame, "#NP", 11);
    klog_error("Segment Not Present (#NP, error code=%p) -- halting",
               reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_ss(InterruptFrame* frame) {
    dump_registers(frame, "#SS", 12);
    klog_error("Stack Fault (#SS, error code=%p) -- halting",
               reinterpret_cast<void*>(frame->error_code));
    fatal_halt();
}

void handle_gp(InterruptFrame* frame) {
    dump_registers(frame, "#GP", 13);

    bool from_user = (frame->cs & 0x03) != 0;

    if (from_user) {
        klog_warn("#GP at RIP=%p from user mode (Ring 3)", reinterpret_cast<void*>(frame->rip));
        klog_warn("Privileged instruction executed in Ring 3 -- protection works");
    } else {
        klog_error("General Protection Fault (#GP) in kernel mode (error code=%p)",
                   reinterpret_cast<void*>(frame->error_code));
    }

    fatal_halt();
}

void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;

    // ---- Stack guard page detection (scheduler task stacks) ----
    {
        auto* cur = cinux::proc::Scheduler::current();
        if (cur != nullptr && cur->kernel_stack_guard_page != 0) {
            uint64_t guard_base = cur->kernel_stack_guard_page;
            uint64_t guard_end  = guard_base + cinux::arch::PAGE_SIZE;
            if (fault_addr >= guard_base && fault_addr < guard_end) {
                kprintf("\n");
                kprintf("========================================================\n");
                kprintf("  KERNEL STACK OVERFLOW DETECTED\n");
                kprintf("========================================================\n");
                kprintf("  Task: tid=%u pid=%d name='%s'\n", cur->tid, cur->pid,
                        cur->name ? cur->name : "(null)");
                kprintf("  Fault address (CR2): %p\n", reinterpret_cast<void*>(fault_addr));
                kprintf("  Guard page range:    [%p, %p)\n", reinterpret_cast<void*>(guard_base),
                        reinterpret_cast<void*>(guard_end));
                kprintf("  Stack range:         [%p, %p)\n",
                        reinterpret_cast<void*>(cur->kernel_stack),
                        reinterpret_cast<void*>(cur->kernel_stack_top));
                kprintf("  Current RSP:         %p\n", reinterpret_cast<void*>(frame->rsp));
                kprintf("  RIP:                 %p\n", reinterpret_cast<void*>(frame->rip));
                kprintf("========================================================\n");
                cinux::lib::kpanic(
                    "kernel stack overflow: task '%s' (tid=%u pid=%d) "
                    "exceeded stack [%p, %p)",
                    cur->name ? cur->name : "(null)", cur->tid, cur->pid,
                    reinterpret_cast<void*>(cur->kernel_stack),
                    reinterpret_cast<void*>(cur->kernel_stack_top));
            }
        }

        // ---- Boot stack overflow detection ----
        // Tests run on the boot stack (no scheduler task).
        // Guard pages between __boot_guard_start and __boot_guard_end
        // are unmapped at test startup.  If the fault address falls in
        // this range, the boot stack has overflowed.
        if (cur == nullptr) {
            uint64_t guard_start = reinterpret_cast<uint64_t>(__boot_guard_start);
            uint64_t guard_end   = reinterpret_cast<uint64_t>(__boot_guard_end);
            if (fault_addr >= guard_start && fault_addr < guard_end) {
                uint64_t boot_stack_top = reinterpret_cast<uint64_t>(__kernel_stack_top);
                kprintf("\n");
                kprintf("========================================================\n");
                kprintf("  BOOT STACK OVERFLOW DETECTED\n");
                kprintf("========================================================\n");
                kprintf("  Fault address (CR2): %p\n", reinterpret_cast<void*>(fault_addr));
                kprintf("  Guard page range:    [%p, %p)\n", reinterpret_cast<void*>(guard_start),
                        reinterpret_cast<void*>(guard_end));
                kprintf("  Boot stack range:    [%p, %p)\n", reinterpret_cast<void*>(guard_end),
                        reinterpret_cast<void*>(boot_stack_top));
                kprintf("  Current RSP:         %p\n", reinterpret_cast<void*>(frame->rsp));
                kprintf("  RIP:                 %p\n", reinterpret_cast<void*>(frame->rip));
                kprintf("========================================================\n");
                cinux::lib::kpanic(
                    "boot stack overflow: fault at %p, "
                    "stack [%p, %p)",
                    reinterpret_cast<void*>(fault_addr), reinterpret_cast<void*>(guard_end),
                    reinterpret_cast<void*>(boot_stack_top));
            }
        }
    }

    // Demand-paging: try to allocate a page for not-present faults
    // Use lock-free allocation paths — the PF handler runs under an
    // Interrupt gate (IF=0) so no concurrent VMM/PMM access is possible
    // on this CPU.  Taking locks here would deadlock on recursive faults.
    if ((err & 0x01) == 0) {
        uint64_t virt_page = fault_addr & ~0xFFFULL;
        uint64_t map_flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;
        if (cinux::arch::is_user_vaddr(fault_addr)) {
            map_flags |= cinux::arch::FLAG_USER;

            auto*           task = cinux::proc::Scheduler::current();
            cinux::mm::VMA* vma  = (task != nullptr && task->addr_space != nullptr)
                                       ? task->addr_space->vmas().find(fault_addr)
                                       : nullptr;

            if (vma == nullptr) {
                // F2-M5: hard VMA gate. A genuine user-mode (err&0x04)
                // not-present fault on an address with no covering VMA is a
                // real segfault -- NULL deref, wild pointer, or stack overflow
                // past the guard. Terminate the offending task. Real SIGSEGV
                // delivery is F3's job; until then exit_current() is the
                // SIGSEGV-equivalent kill, and its context_switch() abandons
                // this frame (the dead task is never resumed here).
                //
                // Kernel-mode access to a user address (ring-0 test code
                // probing a test-built mapping, or copy_to/from_user) is NOT a
                // segfault: keep the legacy zero-page service below so
                // kernel-test PF injection and user-access helpers still work.
                // Read unlocked: PF runs with IF=0 on this single CPU, so no
                // syscall/execve can mutate the VMA store concurrently.
                const bool user_fault = (err & 0x04) != 0;
                if (user_fault && task != nullptr) {
                    klog_error(
                        "segfault: tid=%u '%s' addr=%p has no VMA -- killing task",
                        static_cast<unsigned>(task->tid),
                        task->name ? task->name : "(null)",
                        reinterpret_cast<void*>(fault_addr));
                    cinux::proc::Scheduler::exit_current();
                    // exit_current() switches away and does not return; halt
                    // defensively if it ever does.
                    fatal_halt();
                }
                // Kernel-mode fault or no current task: keep the legacy
                // zero-page service so test/boot PF injection still works.
                klog_warn(
                    "demand-paged user addr %p has no VMA (kernel-mode/no-task "
                    "context; mapping zero page)",
                    reinterpret_cast<void*>(fault_addr));
            } else if (vma->backing != nullptr &&
                       !cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Anonymous)) {
                // File-backed VMA (F2-M4): demand-read the file page through the
                // page cache instead of mapping a zero page, so mmap'd files
                // show real content.  get_page() does the disk read OUTSIDE its
                // own lock, so no I/O happens under a spinlock here (safe at
                // IF=0).  On success map the cached page with the VMA's
                // permissions; on failure fall through to the anonymous zero
                // page so the fault is never left unserved.
                const uint64_t file_off = vma->file_offset + (virt_page - vma->start);
                auto           gp       = cinux::mm::g_page_cache.get_page(vma->backing, file_off);
                if (gp.ok()) {
                    uint64_t fflags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER;
                    if (cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write)) {
                        fflags |= cinux::arch::FLAG_WRITABLE;
                    }
                    // NX (bit 63) is reserved until EFER.NXE is enabled (F9
                    // NX/SMEP/SMAP): setting it now triggers a reserved-bit #PF
                    // on access, so non-exec file pages are not marked NX yet.
                    uint64_t cur_cr3 = cinux::arch::read_cr3();
                    if (g_vmm.map_nolock(virt_page, gp.value()->phys, fflags, &cur_cr3)) {
                        kprintf("[VMM] File demand-read %p -> phys %p\n",
                                reinterpret_cast<void*>(virt_page),
                                reinterpret_cast<void*>(gp.value()->phys));
                        return;
                    }
                } else {
                    klog_warn("page cache read failed for file VMA @ %p",
                              reinterpret_cast<void*>(fault_addr));
                }
                // Fall through to the anonymous zero page below (best effort).
            }
        }
        uint64_t phys = cinux::mm::g_pmm.alloc_page_locked();
        if (phys != 0) {
            uint64_t cur_cr3 = cinux::arch::read_cr3();
            bool     ok      = g_vmm.map_nolock(virt_page, phys, map_flags, &cur_cr3);
            if (ok) {
                kprintf("[VMM] Demand-paged %p -> phys %p\n", reinterpret_cast<void*>(virt_page),
                        reinterpret_cast<void*>(phys));
                return;
            }
            cinux::mm::g_pmm.free_page_locked(phys);
        }
    }

    // CoW fault: page is present but write-protected (fork marks shared pages CoW)
    if ((err & 0x01) && (err & 0x02) && (err & 0x04)) {
        if (cinux::proc::handle_cow_fault(fault_addr)) {
            return;
        }
    }

    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_registers(frame, "#PF", 14);
    klog_error("Page Fault (#PF): %s %s %s%s%s", present, access, mode, reserved, fetch);
    klog_error("Faulting address (CR2) = %p -- halting", reinterpret_cast<void*>(fault_addr));
    fatal_halt();
}

}  // extern "C"
