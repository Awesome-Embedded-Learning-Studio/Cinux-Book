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

#include <stdarg.h>
#include <stdint.h>

#include "arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/backtrace.hpp"
#include "kernel/arch/x86_64/extable.hpp"  // F-EXTABLE: search_exception_tables
#include "kernel/arch/x86_64/fault_diag.hpp"  // F-VERIFY: capture_first_gp/pf + CoW diag (extracted from this file)
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/diagnostics.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"

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


// Central kernel-panic path (FO batch 3): uniform diagnostics for every fatal
// exception and explicit kpanic().  Prints the reason, dumps registers when a
// frame is available, walks + symbolizes the call stack, notes the current
// task, then halts.  Replaces the per-handler dump_registers/klog/fatal_halt
// triplet with a single entry point.
[[noreturn]] void panic(const InterruptFrame* frame, const char* name, uint8_t vector,
                        const char* fmt, ...) {
    kprintf("\n========== KERNEL PANIC ==========\n");
    va_list args;
    va_start(args, fmt);
    cinux::lib::kvprintf(fmt, args);
    va_end(args);
    kprintf("\n");

    if (frame != nullptr) {
        dump_registers(frame, name, vector);
        cinux::arch::backtrace_from(frame->rbp);
    } else {
        cinux::arch::backtrace();
    }

    if (auto* t = cinux::proc::Scheduler::current(); t != nullptr) {
        kprintf("Task: tid=%u pid=%d name='%s'\n", static_cast<unsigned>(t->tid), t->pid,
                t->name ? t->name : "(null)");
    }
    cinux::mm::dump_memory_stats();
    kprintf("==================================\n");
    // isa-debug-exit (port 0xf4): terminate QEMU immediately with a code that
    // names the cause, so CI fails FAST instead of hanging on cli;hlt until the
    // timeout kills QEMU (the old "PF-killed" 2-minute stall). Encoding:
    // value = vector + 2 (value 0=success and 1=test-fail are reserved; value 128
    // also collides with success via (v<<1|1) mod 256, so stay < 128). QEMU exits
    // (value<<1)|1: #DE(0)->5, #DF(8)->21, #GP(13)->31, #PF(14)->33. No-op on bare
    // metal / the production `run` target (no isa-debug-exit device there) --
    // fatal_halt() is the fallback.
    __asm__ volatile("outl %0, $0xf4" : : "a"(static_cast<uint32_t>(vector + 2)));
    fatal_halt();
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
    panic(frame, "#DE", 0, "Divide Error");
}

void handle_nmi(InterruptFrame* frame) {
    panic(frame, "NMI", 2, "Non-maskable Interrupt");
}

void handle_of(InterruptFrame* frame) {
    panic(frame, "#OF", 4, "Overflow");
}

void handle_br(InterruptFrame* frame) {
    panic(frame, "#BR", 5, "BOUND Range Exceeded");
}

void handle_ud(InterruptFrame* frame) {
    panic(frame, "#UD", 6, "Invalid Opcode");
}

void handle_nm(InterruptFrame* frame) {
    panic(frame, "#NM", 7, "Device Not Available");
}

void handle_df(InterruptFrame* frame) {
    panic(frame, "#DF", 8, "Double Fault (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
}

void handle_ts(InterruptFrame* frame) {
    panic(frame, "#TS", 10, "Invalid TSS (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
}

void handle_np(InterruptFrame* frame) {
    panic(frame, "#NP", 11, "Segment Not Present (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
}

void handle_ss(InterruptFrame* frame) {
    panic(frame, "#SS", 12, "Stack Fault (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
}

void handle_gp(InterruptFrame* frame) {
    // F4-M4 M4-2-3 (GOTCHA#25): capture the FIRST faulting frame to debug.log
    // before panic()/current() can recurse on a corrupt %gs. See capture_first_gp.
    capture_first_gp(frame);
    const bool from_user = (frame->cs & 0x03) != 0;
    auto*      task      = cinux::proc::Scheduler::current();
    if (from_user && task != nullptr) {
        // User-mode #GP: an illegal/privileged opcode in ring 3 -- e.g. clang
        // lowering a user-program UB / unreachable path to `hlt` (a 1-byte trap
        // that #GP's in ring 3), or a bad segment selector. The offending USER
        // task must die, NOT the kernel: this aligns with Linux (SIGILL) and the
        // #PF handler's SIGSEGV path below. signal_send queues kSigill; the ISR
        // stub's signal_check_deliver_isr (invoked right after handle_gp returns)
        // delivers it -- to a custom handler if installed, else the default
        // Terminate (its context_switch() abandons this frame). F-ECO batch 0:
        // without this, every user program UB that clang lowers to `hlt` would
        // panic the whole kernel (busybox echo hit this on iter 1).
        uint64_t cr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
        klog_error("#GP user mode: tid=%u '%s' rip=%p cs=%p rsp=%p err=%p cr2=%p -- sending SIGILL",
                   static_cast<unsigned>(task->tid), task->name ? task->name : "(null)",
                   reinterpret_cast<void*>(frame->rip), reinterpret_cast<void*>(frame->cs),
                   reinterpret_cast<void*>(frame->rsp), reinterpret_cast<void*>(frame->error_code),
                   reinterpret_cast<void*>(cr2));
        cinux::proc::signal_send(task, cinux::proc::Signal::kSigill);
        return;
    }
    if (from_user) {
        panic(frame, "#GP", 13, "General Protection Fault from user mode (no current task)");
    }
    panic(frame, "#GP", 13, "General Protection Fault in kernel mode (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
}

void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    // F-EXTABLE: a kernel-mode fault whose RIP is an annotated user accessor
    // (copy_to/from_user rep movsb) recovers via the __ex_table fixup instead
    // of demand-paging or panicking. Rewrite frame->rip to the fixup (clac +
    // ok=false -> caller returns -EFAULT) and return; iretq resumes there.
    // User-mode faults (cs & 3 != 0) skip this and fall through to demand-page
    // unchanged -- the F2 lazy-allocation contract is left to a later milestone.
    if ((frame->cs & 0x03) == 0) {
        if (const auto* entry = cinux::arch::search_exception_tables(frame->rip)) {
            frame->rip = entry->fixup_rip;
            return;
        }
    }

    // F-VERIFY M6: first-fault debugcon capture on PRESENT faults only (err&P --
    // skips benign demand-paging !P so debug.log isn't tagged by the first
    // ordinary demand-fault).  Decodes P/W/U/RSV/I; survives the panic/klog
    // path nesting into another #PF (log3.txt class) via the debugcon channel.
    if (frame->error_code & 0x01) {
        capture_first_pf(frame, fault_addr);
    }

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
                kprintf("  Task: tid=%lu pid=%d name='%s'\n", cur->tid, cur->pid,
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
                    "kernel stack overflow: task '%s' (tid=%lu pid=%d) "
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

            // F9 batch 2: NXE is on -- mark non-executable VMAs NX (W^X). ELF
            // .text (Exec) stays executable; the user stack/heap and non-exec
            // file pages can't run code. Applies to the anonymous fault below
            // (map_flags); file-backed faults set fflags separately.
            if (vma != nullptr && !cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec)) {
                map_flags |= cinux::arch::FLAG_NX;
            }

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
                        "segfault: tid=%u '%s' rip=%p rsp=%p addr=%p has no VMA -- sending SIGSEGV",
                        static_cast<unsigned>(task->tid), task->name ? task->name : "(null)",
                        reinterpret_cast<void*>(frame->rip), reinterpret_cast<void*>(frame->rsp),
                        reinterpret_cast<void*>(fault_addr));
                    // F3-M1: queue SIGSEGV; the ISR stub's signal_check_deliver_isr
                    // (invoked right after handle_pf returns) delivers it -- to a
                    // custom handler if installed, else the default Terminate.
                    cinux::proc::signal_send(task, cinux::proc::Signal::kSigsegv);
                    return;
                }
                // Kernel NULL/near-NULL deref (the nullptr->field pattern): Linux
                // oopses here ("kernel NULL pointer dereference", address <
                // PAGE_SIZE). Demand-paging the zero page would MASK the bug --
                // the kernel reads 0 and continues, crashing later in an
                // unrelated spot (the gui_worker saga: a fault @0x28 was demand-
                // paged to a zero page, then PANIC @0x28 -- root cause eaten).
                // Panic at the deref point with the full frame + backtrace so
                // the offending RIP is obvious instead of a mystery downstream.
                if (fault_addr < 0x1000) {
                    panic(frame, "#PF", 14,
                          "kernel NULL-pointer dereference @ %p (nullptr+0x%lx) -- "
                          "not demand-paging the NULL page (would mask the bug)",
                          reinterpret_cast<void*>(fault_addr), fault_addr);
                }
                // Kernel-mode fault or no current task on a non-NULL user addr:
                // keep the legacy zero-page service so test/boot PF injection
                // and user-access helpers (no exception table -> rely on demand
                // paging) still work.
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
                    // F9 batch 2: NXE is on -- non-exec file pages are NX (bit
                    // 63 is valid now; was reserved-bit #PF before EFER.NXE).
                    if (!cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec)) {
                        fflags |= cinux::arch::FLAG_NX;
                    }
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
            // Anonymous demand pages (brk heap, MAP_ANON, stack growth) must
            // enter user space as zero-filled pages.  PMM pages are recycled
            // raw; leaving stale bytes here corrupts libc allocators and leaks
            // data across mappings.
            memset(reinterpret_cast<void*>(phys_to_virt(phys)), 0, cinux::arch::PAGE_SIZE);
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

    // CoW fault: page is present but write-protected (fork marks shared pages
    // CoW).  Resolve for ANY writer (user OR kernel): CinuxOS syscalls directly
    // dereference user pointers (no copy_to_user yet), so the kernel legitimately
    // writes CoW user pages -- e.g. waitpid storing *status into the parent's
    // fork-CoW'd stack.  handle_cow_fault guards on FLAG_COW, so a genuine
    // read-only page (not CoW) still falls through to panic below.
    if ((err & 0x01) && (err & 0x02)) {
        if (cinux::proc::handle_cow_fault(fault_addr)) {
            return;
        }
        // F-VERIFY M6-2: CoW resolution failed -- dump phys + mapcount to debugcon
        // (lock-free PTE walk; see dump_cow_fail_diagnostic).  Rare path, no noise
        // on the normal CoW-resolved path.
        dump_cow_fail_diagnostic(fault_addr);
    }

    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    panic(frame, "#PF", 14, "Page Fault: %s %s %s%s%s @ CR2=%p", present, access, mode, reserved,
          fetch, reinterpret_cast<void*>(fault_addr));
}

}  // extern "C"
