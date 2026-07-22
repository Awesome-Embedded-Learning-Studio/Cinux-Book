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
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/kprintf.hpp"
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

// ============================================================
// %gs-safe first-fault capture (F4-M4 M4-2-3, GOTCHA#25)
// ============================================================
// A #GP whose root cause is a corrupt (non-canonical) GS_BASE recurses this
// handler: handle_gp -> panic -> Scheduler::current() reads %gs:24 -> the
// non-canonical GS_BASE makes the deref itself #GP -> handle_gp again, ad
// infinitum. The register dump panic() prints is then from a RECURSIVE frame,
// not the real faulting one, so the true faulting RIP is lost. To capture it
// we dump the faulting frame -- plus the live GS/KERNEL_GS MSRs (is GS_BASE
// already non-canonical at the first fault?) -- to the debug console BEFORE
// anything touches %gs:
//   * the frame lives on the stack, reachable via the %rdi argument pointer;
//   * rdmsr needs no %gs;
//   * outb needs no %gs.
// So the dump runs even with a corrupt GS_BASE. Output lands in build/debug.log
// (QEMU -debugcon file, iobase 0xE9). panic() then proceeds as usual; a
// once-flag keeps the recursive frames from flooding the log. This is permanent
// hardening: any future %gs-corrupt #GP leaves a real first-fault RIP instead
// of an unreadable recursive crash.
namespace {
constexpr uint16_t kDebugconPort = 0xE9;

void debugcon_str(const char* s) {
    while (*s != '\0') {
        cinux::io::io_outb(kDebugconPort, static_cast<uint8_t>(*s));
        ++s;
    }
}

void debugcon_hex64(uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    char              buf[19];  // "0x" + 16 hex digits + NUL
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; --i) {
        buf[2 + i] = kHex[v & 0xf];
        v >>= 4;
    }
    buf[18] = '\0';
    debugcon_str(buf);
}

// rdmsr with no %gs dependency (used to read GS_BASE/KERNEL_GS_BASE live).
uint64_t read_msr_raw(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void dump_first_gp(const InterruptFrame* frame) {
    debugcon_str("\n>>> FIRST #GP rip=");
    debugcon_hex64(frame->rip);
    debugcon_str(" rsp=");
    debugcon_hex64(frame->rsp);
    debugcon_str(" cs=0x");
    debugcon_hex64(frame->cs & 0xffff);
    debugcon_str(" err=");
    debugcon_hex64(frame->error_code);
    debugcon_str(" rax(prev)=");
    debugcon_hex64(frame->rax);
    debugcon_str(" rbp=");
    debugcon_hex64(frame->rbp);
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    debugcon_str(" cr3=");
    debugcon_hex64(cr3);
    debugcon_str(" gs_base=");
    debugcon_hex64(read_msr_raw(0xC0000101));
    debugcon_str(" kgs_base=");
    debugcon_hex64(read_msr_raw(0xC0000102));
    debugcon_str(" <<<\n");
}
}  // namespace

// Dumps the first #GP's faulting frame exactly once (recursive frames from a
// corrupt %gs are skipped). Called at the top of handle_gp, before panic().
void capture_first_gp(const InterruptFrame* frame) {
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        dump_first_gp(frame);
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
    if (from_user) {
        panic(frame, "#GP", 13, "General Protection Fault from user mode (Ring 3)");
    }
    panic(frame, "#GP", 13, "General Protection Fault in kernel mode (error code=%p)",
          reinterpret_cast<void*>(frame->error_code));
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
                    klog_error("segfault: tid=%u '%s' addr=%p has no VMA -- sending SIGSEGV",
                               static_cast<unsigned>(task->tid), task->name ? task->name : "(null)",
                               reinterpret_cast<void*>(fault_addr));
                    // F3-M1: queue SIGSEGV; the ISR stub's signal_check_deliver_isr
                    // (invoked right after handle_pf returns) delivers it -- to a
                    // custom handler if installed, else the default Terminate.
                    cinux::proc::signal_send(task, cinux::proc::Signal::kSigsegv);
                    return;
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

    panic(frame, "#PF", 14, "Page Fault: %s %s %s%s%s @ CR2=%p", present, access, mode, reserved,
          fetch, reinterpret_cast<void*>(fault_addr));
}

}  // extern "C"
