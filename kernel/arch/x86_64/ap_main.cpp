/**
 * @file kernel/arch/x86_64/ap_main.cpp
 * @brief Application Processor boot: BSP sequence + AP C entry (F4-M3 Phase 2)
 *
 * BSP (boot_aps) prepares the trampoline at physical 0x8000 (temp page tables +
 * injected params) and drives each AP through INIT-SIPI-SIPI.  The AP runs
 * ap_trampoline.S -> ap_entry_long -> ap_main(), which anchors its GS base,
 * loads its GDT/IDT, enables its LAPIC, signals the BSP, and idles.  APs do not
 * run user tasks yet (M4).
 */

#include <stdint.h>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/irq_backend.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/drivers/acpi/acpi.hpp"
#include "kernel/drivers/apic/local_apic.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

// DEBT-018: ACPI MADT record capacity must cover the runtime CPU limit.  The
// kernel only brings up proc::kMaxCpus APs, but it must record every LAPIC ACPI
// reports; otherwise boot on hw with more LAPICs than kMaxAcpiLapics silently
// truncates the topology.
static_assert(cinux::proc::kMaxCpus <= cinux::drivers::acpi::kMaxAcpiLapics,
              "ACPI LAPIC record capacity must cover the runtime CPU limit");

// Trampoline blob + injected-param symbols (ap_trampoline.S).
extern "C" uint8_t ap_trampoline_start[];
extern "C" uint8_t ap_trampoline_end[];
extern "C" uint8_t ap_stack[];
extern "C" uint8_t ap_entry[];
extern "C" uint8_t ap_cr3[];
extern "C" uint8_t ap_kernel_cr3[];
extern "C" uint8_t ap_cpu_id[];
extern "C" void    ap_entry_long();      // higher-half asm stub (trampoline jumps here)
extern "C" void    usermode_init_asm();  // BSP's STAR/EFER.SCE/SFMASK setup (usermode.S)
extern "C" void    syscall_entry();      // SYSCALL entry (syscall.S); AP writes it into LSTAR

namespace cinux::arch {

namespace {

// Physical address the trampoline is copied to, and the SIPI vector that
// targets it (0x08 -> AP starts real-mode execution at 0x08 << 12 = 0x8000).
constexpr uint64_t kTrampolinePhys = 0x8000;
constexpr uint8_t  kTrampolineVec  = 0x08;
constexpr uint64_t kStackPages     = 4;  // 16 KB AP kernel stack

// APs that have reached ap_main() and signalled online.
uint32_t g_aps_online = 0;

// Rough busy-wait (QEMU is tolerant of INIT-SIPI timing; precision unneeded).
void delay_loops(uint64_t iters) {
    while (iters--) {
        __asm__ volatile("nop");
    }
}

// Write an 8-byte value into the copy of a trampoline param at 0x8000.
// `sym` is the param's kernel-image address; its offset from ap_trampoline_start
// gives its location inside the blob copied to low memory.
void inject_param(void* sym, uint64_t value) {
    uintptr_t off =
        reinterpret_cast<uintptr_t>(sym) - reinterpret_cast<uintptr_t>(ap_trampoline_start);
    auto* dst = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + kTrampolinePhys + off);
    *dst      = value;
}

void copy_trampoline_to_lowmem() {
    auto*     dst  = reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE + kTrampolinePhys);
    uintptr_t size = static_cast<uintptr_t>(ap_trampoline_end - ap_trampoline_start);
    for (uintptr_t i = 0; i < size; i++) {
        dst[i] = ap_trampoline_start[i];
    }
}

// Build temporary page tables (PML4/PDPT/PD) identity-mapping phys 0..64 MB
// and mirroring it at the higher-half kernel image range (0xFFFFFFFF80000000+).
// This lets the AP run the trampoline at 0x8000 (identity) and jump to the
// higher-half ap_entry_long/ap_main.  Mirrors boot/common/long_mode.S
// setup_page_tables.  Returns the temporary PML4 physical address.
uint64_t build_ap_temp_page_tables() {
    uint64_t pml4 = mm::g_pmm.alloc_page();
    uint64_t pdpt = mm::g_pmm.alloc_page();
    uint64_t pd   = mm::g_pmm.alloc_page();

    auto* P4  = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pml4);
    auto* PPT = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pdpt);
    auto* PD  = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pd);
    for (int i = 0; i < 512; i++) {
        P4[i]  = 0;
        PPT[i] = 0;
        PD[i]  = 0;
    }
    P4[0]    = pdpt | 0x3;  // present + writable
    P4[511]  = pdpt | 0x3;  // higher-half mirror shares this PDPT
    PPT[0]   = pd | 0x3;
    PPT[510] = pd | 0x3;  // 0xFFFFFFFF80000000+ (bit 30 = 0)
    for (int i = 0; i < 32; i++) {
        PD[i] = (static_cast<uint64_t>(i) << 21) | 0x83;  // 64 MB, 2 MB pages
    }
    return pml4;
}

}  // namespace

// ============================================================
// AP C entry (called from ap_entry_long after CR3 + stack are set)
// ============================================================
extern "C" void ap_main(uint64_t cpu_id) {
    // 1. Anchor GS at this CPU's PerCpu block so percpu() works.  Kernel-mode
    //    swapgs discipline: GS_BASE = this block, KERNEL_GS_BASE = user (0).
    auto* pcpu   = &proc::percpu_blocks[cpu_id];
    pcpu->cpu_id = static_cast<uint32_t>(cpu_id);
    write_msr(kMsrGsBase, reinterpret_cast<uint64_t>(pcpu));
    write_msr(kMsrKernelGsBase, 0);

    lib::kprintf("[AP%lu] GS anchored\n", cpu_id);

    // 2. Load this CPU's GDT (per-CPU TSS/IST) and the shared IDT.
    gdt_blocks[cpu_id].init();
    g_idt.load();

    // 2b. Configure this CPU's syscall MSRs (STAR / SFMASK / EFER.SCE).  The BSP
    //     does this in usermode_init(); the AP must too, or a user task that
    //     migrates here will #UD on its very first SYSRETQ (EFER.SCE == 0 on the
    //     AP -- the trampoline sets only EFER.LME).  Same class of "AP didn't
    //     match the BSP's CPU config" as the missing CR4.OSFXSR.
    usermode_init_asm();

    // 2c. LSTAR (SYSCALL entry RIP).  usermode_init_asm() sets STAR/SFMASK/
    //     EFER.SCE but NOT LSTAR; only the BSP's syscall_init() wrote it.  An AP
    //     with LSTAR==0 makes a user task's first SYSCALL here load RIP<-0 and
    //     jump to address 0 in kernel mode (CS=STAR[47:32]=0x10), cascading to a
    //     #DF on the user stack (F5-M5 -smp Shell #DF root cause).  Single core
    //     never hit it (the BSP's LSTAR is set).  Point it at syscall_entry.
    constexpr uint32_t kMsrLstar = 0xC0000082;
    write_msr(kMsrLstar, reinterpret_cast<uint64_t>(syscall_entry));

    // 3. Enable this CPU's Local APIC.  The MMIO window the BSP mapped decodes
    //    to whichever CPU accesses it, so g_lapic drives the local one.
    pcpu->apic_id = drivers::apic::g_lapic.id();
    drivers::apic::g_lapic.enable(0xFF);  // spurious vector (matches BSP's 0xFF)

    // 3b. Arm this AP's LAPIC timer for periodic preemption.  The PIT preempts
    //     only the BSP (IOAPIC redirect targets it); without a local timer this
    //     AP would never be preempted, so a task that spin-waits (e.g. the shell
    //     on an empty stdin) deadlocks the AP and starves anything pinned here
    //     (F5-M5 -smp Shell keyboard).  Periodic, /16, count tuned for a
    //     few-hundred-Hz tick -- see lapic_timer_handler (mirrors the PIT path).
    constexpr uint8_t  kTimerDivide = 0x3;        // divide by 16
    constexpr uint32_t kTimerCount  = 200'000;    // ~300 Hz preemption (was 1e6 ~60 Hz ->
                                                  // gui_worker only ~30 Hz = choppy cursor once
                                                  // the usb-tablet tracks precisely)
    drivers::apic::g_lapic.setup_periodic_timer(kLapicTimerVector, kTimerDivide, kTimerCount);

    lib::kprintf("[AP%lu] online (apic_id=%u)\n", cpu_id, pcpu->apic_id);

    // 4. Signal the BSP that this AP is up.
    __atomic_add_fetch(&g_aps_online, 1, __ATOMIC_SEQ_CST);

    // 5. (F4-M4 M4-2-2) This AP now participates in scheduling.  boot_aps()
    //    runs on the BSP BEFORE Scheduler::init() (main.cpp boot_aps ~:205 vs
    //    Scheduler::init ~:249), so the scheduler is not ready yet.  Spin until
    //    it is -- cli;pause, NOT sti;hlt: init() sends no IPI, so hlt would
    //    sleep forever.  init() finishes in microseconds, so the spin is brief.
    __asm__ volatile("cli");
    while (!proc::Scheduler::is_initialized()) {
        __asm__ volatile("pause");
    }

    // 6. Build this AP's idle task (entry = ap_idle_entry) and become it.
    //    current() must be the idle task before the first context_switch so that
    //    schedule()'s `current()==nullptr` early-out does not fire and prev
    //    resolves to idle.  setup_ap_idle() is idempotent.
    proc::Task* ap_idle = proc::Scheduler::setup_ap_idle(static_cast<uint32_t>(cpu_id));
    pcpu->current       = ap_idle;

    // 7. First context switch into ap_idle_entry.  `dummy` is write-only
    //    (context_switch saves callee-saved regs + .restore rip + fs_base into
    //    it, never reads it), so leaving it uninitialised is safe.  The AP never
    //    returns here: context_switch jumps to ap_idle_entry on the idle task's
    //    own stack, abandoning ap_main's stack (allocated by boot_aps, 16 KB,
    //    never freed or reused -- harmless).  See GOTCHA#23: after the switch
    //    back, ap_idle_entry reads current() (per-CPU), never this local.
    proc::CpuContext dummy;
    context_switch(&dummy, &ap_idle->ctx);
    // unreachable -- ap_idle_entry loops forever; guard just in case.
    while (true) {
        __asm__ volatile("hlt");
    }
}

// ============================================================
// BSP: boot all APs from the ACPI MADT
// ============================================================
void boot_aps() {
    auto& info = drivers::acpi::g_acpi_info;
    if (info.cpu_count <= 1) {
        lib::kprintf("[SMP] single CPU (cpu_count=%u); no APs to boot\n", info.cpu_count);
        return;
    }
    lib::kprintf("[SMP] booting AP(s); cpu_count=%u\n", info.cpu_count);

    // Prepare the trampoline once (static params are shared across APs).
    uint64_t temp_cr3   = build_ap_temp_page_tables();
    uint64_t kernel_cr3 = mm::AddressSpace::kernel_pml4();
    copy_trampoline_to_lowmem();
    inject_param(ap_entry, reinterpret_cast<uint64_t>(&ap_entry_long));
    inject_param(ap_cr3, temp_cr3);
    inject_param(ap_kernel_cr3, kernel_cr3);

    uint32_t bsp_apic_id = drivers::apic::g_lapic.id();
    uint32_t ap_cpu_idx  = 1;  // BSP owns percpu_blocks[0]
    uint32_t booted      = 0;

    for (uint32_t i = 0; i < info.cpu_count && i < proc::kMaxCpus; i++) {
        uint8_t apic_id = info.cpu_apic_ids[i];
        if (apic_id == bsp_apic_id) {
            continue;  // skip the BSP
        }

        // Per-AP kernel stack (direct-map virt; mapped in the kernel PML4).
        uint64_t stack_phys = mm::g_pmm.alloc_pages(kStackPages);
        if (stack_phys == 0) {
            lib::kprintf("[SMP] stack alloc failed for apic_id %u\n", apic_id);
            continue;
        }
        uint64_t stack_top = DIRECT_MAP_BASE + stack_phys + kStackPages * PAGE_SIZE;
        inject_param(ap_stack, stack_top);
        inject_param(ap_cpu_id, static_cast<uint64_t>(ap_cpu_idx));

        uint32_t before = __atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST);
        lib::kprintf("[SMP] INIT-SIPI-SIPI -> apic_id %u (cpu %u)\n", apic_id, ap_cpu_idx);

        // INIT - delay - SIPI - delay - SIPI (Intel MP startup sequence).  The
        // second SIPI is a safety net: send it only if the first did not already
        // bring the AP up (avoids a spurious trampoline re-run + fault once the
        // AP is already online and halted).
        drivers::apic::g_lapic.send_init(apic_id);
        delay_loops(200000);
        drivers::apic::g_lapic.send_sipi(apic_id, kTrampolineVec);
        delay_loops(50000);
        if (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) == before) {
            drivers::apic::g_lapic.send_sipi(apic_id, kTrampolineVec);
        }

        // Wait for this AP to signal online (bounded spin).
        uint32_t spin = 0;
        while (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) == before) {
            if (++spin > 500000000) {
                lib::kprintf("[SMP] AP apic_id %u did not come up\n", apic_id);
                break;
            }
            __asm__ volatile("pause");
        }
        if (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) > before) {
            booted++;
        }
        ap_cpu_idx++;
    }

    lib::kprintf("[SMP] %u AP(s) online\n", booted);
}

// ============================================================
// Reschedule IPI (F4-M4 M4-2): wake an idle AP so it re-checks the shared
// run queue.  Once M4-2-2 switches the AP idle loop to sti;hlt, an IPI here
// pulls the AP out of hlt and its loop runs schedule() again.
// ============================================================

// The handler body is empty: the ISR_IRQ stub sends the EOI.  The actual
// reschedule happens in the AP's idle loop after this stub returns control to
// the hlt instruction.
extern "C" void reschedule_ipi_handler(InterruptFrame* /*frame*/) {
    // EOI is sent by the ISR_IRQ stub.
}

// ============================================================
// LAPIC timer tick (F5-M5 -smp): each AP's periodic preemption source.
// ============================================================
// The PIT preempts only the BSP (the IOAPIC redirect targets the BSP), so APs
// arm their own LAPIC timer (ap_main) and land here.  This mirrors the PIT path
// (pit_irq0_handler): an EARLY EOI before Scheduler::tick() so the timer is
// re-armed ahead of tick's inline preemption switch (otherwise the next tick
// could be missed on the task we switch to).  tick() runs the per-class
// preemption policy and may context-switch.
extern "C" void lapic_timer_handler(InterruptFrame* /*frame*/) {
    cinux::arch::irq_eoi(0);  // EOI the LAPIC timer before the preempt switch
    cinux::proc::Scheduler::tick();
}

void wake_idle_ap() {
    // Best-effort wake: IPI every AP that has signalled online.  A redundant
    // IPI to an AP that is already busy is harmless -- its idle loop re-checks
    // the run queue and halts again if empty.  This trades a few spurious
    // wakeups for not needing a precisely-tracked per-CPU idle flag (whose
    // reader-on-BSP / writer-on-AP race would need its own care).  Single-core
    // systems have g_aps_online == 0, so the loop never runs -> no-op.
    uint32_t online = __atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST);
    for (uint32_t cpu = 1; cpu <= online && cpu < proc::kMaxCpus; cpu++) {
        // percpu_blocks[cpu].apic_id was stored by that AP *before* it incremented
        // g_aps_online (acquire/release via SEQ_CST), so it is visible once we
        // observe online >= cpu.  id() returns a plain APIC id; send_ipi shifts
        // it into ICR[bits 24-31], matching.
        uint8_t apic_id = static_cast<uint8_t>(proc::percpu_blocks[cpu].apic_id);
        drivers::apic::g_lapic.send_ipi(apic_id, kRescheduleIpiVector);
    }
}

}  // namespace cinux::arch
