/**
 * @file kernel/arch/x86_64/page_fault.cpp
 * @brief #PF handler (split from exception_handlers.cpp for the 500-line limit)
 *
 * Demand paging (anonymous + file-backed via the page cache), copy-on-write,
 * uaccess exception-table recovery, stack-overflow detection, and the
 * SIGSEGV-vs-panic policy: an unresolvable USER #PF delivers SIGSEGV (Linux
 * behaviour); only KERNEL #PF panics.  Called by the ISR stub in interrupts.S.
 */

#include <stdint.h>

#include <cstring>  // memcpy/memset

#include "arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/extable.hpp"     // search_exception_tables
#include "kernel/arch/x86_64/fault_diag.hpp"  // capture_first_pf + panic + CoW diag
#include "kernel/arch/x86_64/idt.hpp"         // InterruptFrame
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/address_space.hpp"
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

constexpr uint64_t kPageFaultWrite       = 1u << 1;
constexpr uint64_t kPageFaultInstruction = 1u << 4;

bool vma_allows_fault(cinux::mm::VMA* vma, uint64_t error_code) {
    if (vma == nullptr) {
        return false;
    }
    if ((error_code & kPageFaultInstruction) != 0) {
        return cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec);
    }
    if ((error_code & kPageFaultWrite) != 0) {
        return cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write);
    }
    return cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Read) ||
           cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write);
}

// Cumulative #PF count for ad-hoc profiling (B1 gcc-compile-stutter).  handle_pf
// runs at IF=0 but multiple CPUs can fault concurrently under -smp 2, so a plain
// ++ would race; an atomic add is cheap and correct.
uint64_t g_pf_count = 0;
}  // namespace

// Total #PF since boot (atomically bumped by handle_pf).  Defined next to the
// counter; declared in fault_diag.hpp so dump_memory_stats can read it without
// dragging in the whole PF handler.
uint64_t pf_count() {
    return __atomic_load_n(&g_pf_count, __ATOMIC_RELAXED);
}

extern "C" {

void handle_pf(InterruptFrame* frame) {
    __atomic_fetch_add(&g_pf_count, 1, __ATOMIC_RELAXED);
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    // F-EXTABLE: a kernel-mode fault whose RIP is an annotated user accessor
    // (copy_to/from_user rep movsb) normally recovers via the __ex_table fixup
    // (clac + ok=false -> caller returns -EFAULT).  One case must NOT be fixed
    // up immediately: a not-present fault on a valid user VMA.  Linux uaccess
    // faults can populate a lazy user page and then resume the rep movsb; doing
    // the fixup first turns large read()/write() buffers that cross an
    // untouched malloc/mmap page into spurious -EFAULT.
    const auto* extable_entry =
        ((frame->cs & 0x03) == 0) ? cinux::arch::search_exception_tables(frame->rip) : nullptr;
    if (extable_entry != nullptr) {
        bool should_demand_page = false;
        if ((frame->error_code & 0x01) == 0 && cinux::arch::is_user_vaddr(fault_addr)) {
            auto* task = cinux::proc::Scheduler::current();
            should_demand_page =
                task != nullptr && task->addr_space != nullptr &&
                vma_allows_fault(task->addr_space->vmas().find(fault_addr), frame->error_code);
        }
        if (!should_demand_page) {
            const auto* entry = extable_entry;
            frame->rip        = entry->fixup_rip;
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

            if (!vma_allows_fault(vma, err)) {
                // F2-M5: hard VMA gate. A genuine user-mode (err&0x04)
                // not-present fault on an address with no covering VMA is a
                // real segfault -- NULL deref, wild pointer, stack overflow
                // past the guard, or access to a PROT_NONE / wrong-permission
                // mapping. Terminate the offending task.
                //
                // Kernel-mode access to a user address (ring-0 test code
                // probing a test-built mapping, or copy_to/from_user) is NOT a
                // segfault: keep the legacy zero-page service below so
                // kernel-test PF injection and user-access helpers still work.
                // Read unlocked: PF runs with IF=0 on this single CPU, so no
                // syscall/execve can mutate the VMA store concurrently.
                const bool user_fault = (err & 0x04) != 0;
                if (user_fault && task != nullptr) {
                    if (vma != nullptr) {
                        klog_error("segfault VMA: [%p, %p) flags=0x%lx err=0x%lx",
                                   reinterpret_cast<void*>(vma->start),
                                   reinterpret_cast<void*>(vma->end),
                                   static_cast<unsigned long>(vma->flags),
                                   static_cast<unsigned long>(err));
                    } else {
                        klog_error("segfault VMA: none err=0x%lx", static_cast<unsigned long>(err));
                    }
                    klog_error(
                        "segfault: tid=%u '%s' rip=%p rsp=%p addr=%p not permitted -- sending "
                        "SIGSEGV",
                        static_cast<unsigned>(task->tid), task->name ? task->name : "(null)",
                        reinterpret_cast<void*>(frame->rip), reinterpret_cast<void*>(frame->rsp),
                        reinterpret_cast<void*>(fault_addr));
                    // F3-M1: queue SIGSEGV; the ISR stub's signal_check_deliver_isr
                    // (invoked right after handle_pf returns) delivers it -- to a
                    // custom handler if installed, else the default Terminate.
                    cinux::proc::signal_force_send(task, cinux::proc::Signal::kSigsegv);
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
            } else if (vma != nullptr &&
                       cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::IoPhys)) {
                // F-GUI-USERSPACE batch 1: device mmap (e.g. /dev/fb0).  Map the
                // pre-bound physical page verbatim with FLAG_PCD (uncached, like
                // every other MMIO region) -- NO PMM allocation, NO page cache,
                // NO pte_count_inc: device memory is not PMM-managed, so both
                // teardown (munmap) and fork skip it.  W^X: non-exec VMAs get
                // FLAG_NX, writable VMAs get FLAG_WRITABLE.
                const uint64_t io_phys = vma->phys_base + (virt_page - vma->start);
                uint64_t       ioflags =
                    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER | cinux::arch::FLAG_PCD;
                if (cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write)) {
                    ioflags |= cinux::arch::FLAG_WRITABLE;
                }
                if (!cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec)) {
                    ioflags |= cinux::arch::FLAG_NX;
                }
                uint64_t cur_cr3 = cinux::arch::read_cr3();
                if (g_vmm.map_nolock(virt_page, io_phys, ioflags, &cur_cr3)) {
                    return;
                }
                // map_nolock failure = an intermediate page-table page could not
                // be allocated (OOM).  Do NOT fall through to the anonymous
                // service: that would back this device VMA with a fresh RAM page
                // instead of device memory, corrupting the mapping.  Leave the
                // fault unserved; the retry re-enters this branch (OOM resolves
                // or the system panics, same outcome as anonymous OOM).
                return;
            } else if (vma != nullptr && vma->backing != nullptr &&
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
                    // F9 batch 2: NXE is on -- non-exec file pages are NX (bit
                    // 63 is valid now; was reserved-bit #PF before EFER.NXE).
                    if (!cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Exec)) {
                        fflags |= cinux::arch::FLAG_NX;
                    }

                    // MAP_PRIVATE writable file mappings (a shared library's
                    // .data/.got) must NOT map the page-cache page writably:
                    // that page is shared across every process mapping the file,
                    // so a write -- the dynamic loader relocating DT_GNU_HASH or
                    // GOT slots -- corrupts the cached contents for the next
                    // loader. (Root cause of the ld SIGSEGV on the GCC self-host
                    // line: `as` relocated libbfd's DYNAMIC d_ptr straight into
                    // the shared cache page; `ld` later hit that same cache page
                    // and dereferenced `as`'s now-stale relocation.)  Copy the
                    // cached page into a fresh private page (copy-on-demand) and
                    // map THAT writably, leaving the cache page untouched.
                    // Read-only and MAP_SHARED file mappings keep sharing it.
                    const bool private_writable =
                        cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write) &&
                        !cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Shared);
                    uint64_t map_phys = gp.value()->phys;
                    if (private_writable) {
                        const uint64_t cow_phys = cinux::mm::g_pmm.alloc_page();
                        if (cow_phys != 0) {
                            memcpy(reinterpret_cast<void*>(cinux::arch::DIRECT_MAP_BASE + cow_phys),
                                   reinterpret_cast<void*>(cinux::arch::DIRECT_MAP_BASE +
                                                           gp.value()->phys),
                                   cinux::arch::PAGE_SIZE);
                            map_phys = cow_phys;
                            fflags |= cinux::arch::FLAG_WRITABLE;
                        }
                        // The cache page is no longer mapped into this VMA --
                        // drop the reference get_page() took.  On OOM (cow_phys
                        // == 0) fall through mapping the cache page read-only:
                        // reads still work, the next write re-faults, and the
                        // shared cache page is never corrupted.
                        cinux::mm::g_page_cache.release(gp.value());
                    } else if (cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::Write)) {
                        // MAP_SHARED writable file mapping: writes hit the shared
                        // cache page (write-back to disk is a follow-up).
                        fflags |= cinux::arch::FLAG_WRITABLE;
                    }

                    uint64_t cur_cr3 = cinux::arch::read_cr3();
                    if (g_vmm.map_nolock(virt_page, map_phys, fflags, &cur_cr3)) {
                        // batch 3: every install PTE bumps the mapping counter
                        // so teardown (pte_count_dec_and_test) can reach 0 and
                        // drop the ownership ref.  Mapping the page-cache page
                        // also takes a map-ownership refcount -- the cache's own
                        // ref (set by CachePhysRef::alloc) survives teardown so
                        // the page is not freed while still cached (the ld/crt1
                        // corruption root cause).  A private CoW copy takes no
                        // extra refcount: its alloc baseline IS its ownership.
                        cinux::mm::g_pmm.pte_count_inc(map_phys);
                        if (map_phys == gp.value()->phys) {
                            cinux::mm::g_pmm.refcount_inc(map_phys);
                        }
                        return;
                    }
                    if (map_phys != gp.value()->phys) {
                        // batch 4: roll back the private CoW copy via its
                        // ownership ref (alloc set refcount=1; dec -> 0 -> free).
                        cinux::mm::g_pmm.refcount_dec_and_test(map_phys);
                    }
                } else {
                    // File-backed page read failed (I/O error or driver
                    // mis-read).  A user-mode access gets SIGBUS (Linux
                    // semantics) so the offender dies with the real cause --
                    // NOT a silently-mapped zero page, which serves the fault
                    // with wrong data and, if the page is later executed,
                    // runs user code on stale/garbage bytes (the FC29000
                    // #UD-on-garbage path).  Kernel mode (e.g. an extable
                    // accessor) still falls through to the zero page as a
                    // best-effort last resort.
                    klog_error("file page read failed @ %p backing=%p off=0x%lx -- %s",
                               reinterpret_cast<void*>(fault_addr),
                               reinterpret_cast<void*>(vma->backing),
                               static_cast<unsigned long>(file_off),
                               (err & 0x04) ? "user mode, sending SIGBUS"
                                            : "kernel mode, mapping zero page");
                    if ((err & 0x04) != 0) {
                        auto* offender = cinux::proc::Scheduler::current();
                        if (offender != nullptr) {
                            cinux::proc::signal_force_send(offender, cinux::proc::Signal::kSigbus);
                            return;
                        }
                    }
                    // Kernel mode or no task: fall through to anonymous zero
                    // page below (best effort).
                }
            }
        }
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        if (phys != 0) {
            // Anonymous demand pages (brk heap, MAP_ANON, stack growth) must
            // enter user space as zero-filled pages.  PMM pages are recycled
            // raw; leaving stale bytes here corrupts libc allocizers and leaks
            // data across mappings.
            memset(reinterpret_cast<void*>(phys_to_virt(phys)), 0, cinux::arch::PAGE_SIZE);
            uint64_t cur_cr3 = cinux::arch::read_cr3();
            bool     ok      = g_vmm.map_nolock(virt_page, phys, map_flags, &cur_cr3);
            if (ok) {
                cinux::mm::g_pmm.pte_count_inc(phys);  // batch 3: account for the new PTE
                return;
            }
            cinux::mm::g_pmm.refcount_dec_and_test(
                phys);  // batch 4: roll back alloc via ownership ref
        }
    }

    // CoW fault: page is present but write-protected (fork marks shared pages
    // CoW).  Resolve for ANY writer (user OR kernel): Cinux syscalls directly
    // dereference user pointers (no copy_to_user yet), so the kernel legitimately
    // writes CoW user pages -- e.g. waitpid storing *status into the parent's
    // fork-CoW'd stack.  handle_cow_fault guards on FLAG_COW, so a genuine
    // read-only page (not CoW) still falls through to panic below.
    if ((err & 0x01) && (err & 0x02)) {
        if (cinux::proc::handle_cow_fault(fault_addr)) {
            return;
        }
        // F-VERIFY M6-2: CoW resolution failed -- dump phys + pte_count to debugcon
        // (lock-free PTE walk; see dump_cow_fail_diagnostic).  Rare path, no noise
        // on the normal CoW-resolved path.
        dump_cow_fail_diagnostic(fault_addr);
    }

    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    // User-mode fault we could NOT resolve (genuine write to a read-only page,
    // reserved bits, instruction fetch on an NX page, ...): deliver SIGSEGV and
    // keep the kernel alive.  Linux kills the offending process here -- it does
    // NOT panic.  The panic below is reserved for KERNEL-mode faults (a real
    // kernel bug worth halting on).  Without this, a corrupt user pointer that
    // writes a RO page (the ld-on-cc1-.o saga: a dangling VMA backing inode
    // served a zero page; ld executed `00 00` = add %al,(%rax) -> write-protect)
    // took the whole kernel down instead of just SIGSEGV-ing the linker.
    if ((err & 0x04) != 0) {
        auto* task = cinux::proc::Scheduler::current();
        if (task != nullptr) {
            cinux::mm::VMA* vma =
                task->addr_space != nullptr ? task->addr_space->vmas().find(fault_addr) : nullptr;
            if (vma != nullptr) {
                klog_error("segfault VMA: [%p, %p) flags=0x%lx backing=%p file_off=0x%lx",
                           reinterpret_cast<void*>(vma->start), reinterpret_cast<void*>(vma->end),
                           static_cast<unsigned long>(vma->flags),
                           reinterpret_cast<void*>(vma->backing),
                           static_cast<unsigned long>(vma->file_offset));
            } else {
                klog_error("segfault VMA: none");
            }
            klog_error(
                "segfault: tid=%u '%s' rip=%p rsp=%p addr=%p err=0x%lx (%s %s%s%s) "
                "-- unresolvable user #PF, sending SIGSEGV",
                static_cast<unsigned>(task->tid), task->name ? task->name : "(null)",
                reinterpret_cast<void*>(frame->rip), reinterpret_cast<void*>(frame->rsp),
                reinterpret_cast<void*>(fault_addr), static_cast<unsigned long>(err), access,
                present, reserved, fetch);
            cinux::proc::signal_force_send(task, cinux::proc::Signal::kSigsegv);
            return;
        }
    }

    panic(frame, "#PF", 14, "Page Fault: %s %s %s%s%s @ CR2=%p", present, access, mode, reserved,
          fetch, reinterpret_cast<void*>(fault_addr));
}

}  // extern "C"
