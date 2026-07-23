/**
 * @file kernel/main.cpp
 * @brief Big kernel entry point
 *
 * This is the C++ main function for the "big kernel" -- the full-featured
 * kernel that the mini kernel loads from disk and jumps to.
 *
 * Milestone 025 goal:
 *   [AHCI] Read sector 0: 55 AA
 *
 * Milestone 026 goal:
 *   [RAMDISK] List files in embedded initrd (ustar)
 *
 * Initialisation order:
 *   1. Serial port (kprintf serial sink)
 *   2. GDT (segment descriptors + TSS with IST1 Double Fault stack)
 *   3. IDT (CPU exception vectors 0-14, #DF uses IST1)
 *   4. PIC (remap IRQ0-15 to vectors 0x20-0x2F)
 *   5. IRQ handlers (register IRQ stubs in IDT)
 *   6. PIT (configure channel 0 at 100 Hz)
 *   7. PMM (physical memory manager, bitmap allocator)
 *   8. VMM (virtual memory manager, 4-level paging)
 *   9. AddressSpace (save kernel PML4 for per-process spaces)
 *  10. Heap (kernel heap allocator, first-fit with coalescing)
 *  11. Framebuffer (map MMIO, init from BootInfo)
 *  12. Font (parse embedded PSF2)
 *  13. Console (init + register as kprintf sink)
 *  14. Keyboard (PS/2 controller init)
 *  15. Unmask IRQ0 + IRQ1, enable interrupts (sti)
 *  16. Usermode init (STAR/EFER MSRs for SYSRET)
 *  17. Syscall init (LSTAR, SFMASK, GS base, handler registration)
 *  18. Launch first user-mode program (Ring 3)
 *  19. PCI enumeration
 *  20. AHCI init + read sector 0 (MBR signature test)
 *  21. Ramdisk mount (parse embedded ustar initrd, list files)
 */

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/arch/x86_64/extable.hpp"  // F-EXTABLE: sort_extable at boot
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/irq_backend.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"  // F9 batch 3: enable_smep
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/acpi/acpi.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/net/e1000_init.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/rtc/rtc.hpp"
#include "kernel/drivers/tty/console_tty.hpp"
#include "kernel/drivers/video/console.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"
#include "kernel/lib/kallsyms.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/random.hpp"  // F9 batch 7: g_random
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/net/net_init.hpp"
#include "kernel/proc/init.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/userspace.hpp"  // handoff_framebuffer_to_gui (§14: GUI init or no-op stub)

using cinux::arch::PIC;
using cinux::drivers::Console;
using cinux::drivers::Framebuffer;
using cinux::drivers::Keyboard;
using cinux::drivers::PIT;
using cinux::drivers::PSFFont;
using cinux::proc::Scheduler;
using cinux::proc::TaskBuilder;

// BootInfo is placed at physical 0x7000 by the bootloader
static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

// Forward declarations for IRQ init (defined in irq_handlers.cpp)
extern "C" void irq_init();


/**
 * @brief Big kernel main entry point
 *
 * Called from boot.S after BSS clear and global ctors.
 *
 * @return This function should never return; the halt loop in
 *         boot.S catches it if it does.
 */
extern "C" void kernel_main() {
    // Step 1: Initialise the serial port used by kprintf
    cinux::lib::kprintf_init();

    // F-INFRA I-5: register the build-generated symbol table so panic backtraces
    // resolve to function names without host addr2line. Safe at IF=0 (no locks).
    cinux::lib::kallsyms_set_table(g_kallsyms_table, g_kallsyms_count);

    // Step 2: Print the milestone message
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    // Step 3: Initialise the GDT (must come before IDT)
    cinux::arch::gdt_blocks[0].init();
    cinux::lib::kprintf("[BIG] GDT loaded (TSS with IST1 Double Fault stack).\n");

    // Step 4: Initialise the IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded (#DF uses IST1).\n");

    // F4-M3 P1-2 (early): anchor the BSP's GS base at its PerCpu block now, so
    // percpu() (which reads MSR_GS_BASE) works before any interrupt fires or any
    // code uses it.  Also configures STAR/EFER for SYSRET (formerly Step 18).
    cinux::arch::usermode_init();
    cinux::arch::enable_smep_smap();  // F9 batch 3/4: SMEP+SMAP on the BSP (CR4 per-CPU)
    cinux::lib::kprintf("[BIG] PerCpu GS base anchored (BSP).\n");

    // Step 5: Initialise the PIC (remap IRQ0-7 -> 0x20-0x27,
    //         IRQ8-15 -> 0x28-0x2F, all masked)
    PIC::init();
    cinux::lib::kprintf("[BIG] PIC initialised.\n");

    // Step 6: Register IRQ handlers in the IDT (vectors 0x20-0x2F)
    irq_init();

    // F-EXTABLE: sort the __ex_table (user-accessor fixup sites) by fault_rip so
    // handle_pf can binary-search it. IDT is up and interrupts are still off, so
    // no accessor fault can fire yet (no user program). Safe no-op while empty.
    cinux::arch::sort_extable();

    // Step 7: Initialise PIT channel 0 at 100 Hz (10 ms per tick)
    PIT::init(100);

    // F9 batch 7: seed the kernel PRNG (rdrand/TSC/PIT entropy) for ASLR (b8).
    cinux::lib::g_random.init();

    // Step 7b: Parse ACPI (RSDP/MADT) for the APIC base addresses and CPU list
    // that M2 consumes.  Only needs the loader's direct map, which is already up.
    cinux::drivers::acpi::init();

    // Step 8: Trigger a software breakpoint to verify exception
    // handling still works after PIC/IRQ setup
    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    // Step 9: Initialise Physical Memory Manager
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    cinux::mm::g_pmm.init(*boot_info);

    // Step 10: Initialise Virtual Memory Manager
    cinux::mm::g_vmm.init();

    // Step 11: Save kernel PML4 for per-process address spaces
    cinux::mm::AddressSpace::init_kernel();

    // Step 11b: HPET high-res monotonic + RTC wall clock (F5-M4).  HPET needs VMM
    // (it maps its MMIO window) and ACPI (find_table); RTC is pure port I/O.
    // Both are up by here.  These feed sys_clock_gettime: MONOTONIC <- HPET
    // (PIT fallback), REALTIME <- RTC boot epoch + HPET monotonic delta.
    cinux::drivers::g_hpet.init();
    cinux::drivers::g_rtc.init();

    // Step 12: Initialise the slab allocator (small objects) + kmalloc.  Large
    // allocations reuse the direct map, so only the slab window is reserved.
    cinux::mm::g_slab.init(cinux::arch::KMEM_SLAB_BASE, cinux::arch::KMEM_SLAB_SIZE);
    cinux::mm::init_dedicated_caches();  // F2-M7b: task / vma / cached_page caches

    // Step 12b: Initialise the file-backed page cache (F2-M4).  Advisory 10%
    // ceiling; eviction is deferred.  Needs the slab for CachedPage nodes.
    cinux::mm::g_page_cache.init(cinux::mm::g_pmm.free_page_count() / 10);

    // Step 13: Initialise framebuffer from BootInfo
    Framebuffer fb;
    fb.init(*boot_info);
    cinux::lib::kprintf("[BIG] Framebuffer initialised: %ux%u %ubpp\n", fb.width(), fb.height(),
                        boot_info->fb_bpp);

    // Step 14: Parse embedded PSF2 font
    static PSFFont font;
    font.init();
    cinux::lib::kprintf("[BIG] PSF2 font loaded: %ux%u\n", font.width(), font.height());

    // Step 15: Initialise text console and register as kprintf sink
    Console console;
    console.init(fb, font, 0x00FFFFFF, 0x00000000);
    cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
    cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");

    // F10-M3 batch 2: wire the console TTY (stdin line discipline + echo sink)
    // before the keyboard starts delivering IRQs.
    cinux::drivers::console_tty().init();

    // Step 15b: hand the framebuffer + console off to the GUI (canvas + window
    // manager init; console detached so routine logs stop overlaying the
    // desktop).  No-op when GUI is compiled out (§14 stub linked).  kpanic
    // re-enables all sinks, so a crash still reaches the screen.
    cinux::proc::handoff_framebuffer_to_gui(fb, font, console);

    // Step 16: Initialise the PS/2 keyboard controller
    Keyboard::init();

    // Step 17: Switch from the 8259 PIC to the APIC (mask PIC, enable LAPIC,
    // route ISA IRQ 0/1/12 onto vectors 0x20/0x21/0x2C via the I/O APIC), then
    // enable interrupts.  Falls back to PIC if ACPI/APIC init fails.
    cinux::arch::switch_to_apic();
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled.\n");

    // Step 17b: Boot Application Processors (F4-M3 P2).  Bring each AP through
    // INIT-SIPI-SIPI to 64-bit long mode; they reach ap_main(), signal online,
    // then halt (no tasks until M4).  No-op on a single-CPU machine.
    cinux::arch::boot_aps();

    // Step 18: user-mode STAR/EFER support was initialised early (right after
    // the IDT) so the BSP's GS base is anchored before interrupts are enabled.

    // Step 19: Initialise syscall infrastructure (LSTAR, SFMASK, dispatch table)
    cinux::arch::syscall_init();

    // Step 20: PCI enumeration
    cinux::lib::kprintf("[BIG] ===== Milestone 025: AHCI Driver =====\n");
    cinux::drivers::pci::PCI pci;
    pci.init();

    // Step 21: Find AHCI controller and initialise
    static cinux::drivers::ahci::AHCI ahci;
    cinux::drivers::pci::PCIDevice    ahci_dev;
    if (pci.find_ahci(ahci_dev)) {
        ahci.init(ahci_dev);
        cinux::drivers::ahci::AHCI::set_instance(&ahci);

        // Step 22: Read sector 0 (MBR) and check boot signature
        uint64_t buf_phys = cinux::mm::g_pmm.alloc_page();
        if (buf_phys != 0) {
            constexpr uint64_t buf_virt  = cinux::arch::KMEM_DMA_BASE;
            constexpr uint64_t buf_flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;
            cinux::mm::g_vmm.map(buf_virt, buf_phys, buf_flags);

            auto* buf = reinterpret_cast<uint8_t*>(buf_virt);
            for (uint32_t i = 0; i < 512; ++i) {
                buf[i] = 0;
            }

            if (ahci.read(0, 0, 1, buf_phys)) {
                cinux::lib::kprintf("[AHCI] Read sector 0: %02x %02x\n", buf[510], buf[511]);
            } else {
                cinux::lib::kprintf("[AHCI] Failed to read sector 0.\n");
            }
        }
    } else {
        cinux::lib::kprintf("[AHCI] No AHCI controller found.\n");
    }

    // Step 21b: Bring up the e1000 NIC (if present).  §14 file gate: net::init()
    // is a no-op stub when CINUX_NET is off, so this call needs no #ifdef.
    cinux::drivers::net::init();

    // Step 21c: Bring up the L3 stack (ARP / IPv4 / ICMP) over the NIC -- the
    // composition root that builds the adapter + NetStack and attaches them.
    // §14 file gate: no-op stub when CINUX_NET is off, so no #ifdef here.
    cinux::net::init();

    // Step 22: Initialise scheduler and spawn kernel init thread
    cinux::lib::kprintf("[BIG] ===== Scheduler & Init Thread =====\n");
    Scheduler::init();

    // F7 follow-up: start the resident net RX poll-driver kthread (sti/hlt +
    // NetStack::poll). Must come after the scheduler is up; queued here, it runs
    // once run_first() begins dispatch. Lets ping() yield (default pump) instead
    // of sti/hlt-ing inside the syscall (the #DF hazard). No-op if no NIC.
    cinux::net::start_poll_driver();

    auto* init_task =
        TaskBuilder().set_entry(cinux::proc::kernel_init_thread).set_name("kernel_init").build();
    if (init_task != nullptr) {
        Scheduler::add_task(init_task);
    }

    auto* boot_task = TaskBuilder()
                          .set_entry([]() {
                              cinux::lib::kprintf("[BOOT] boot_task_entry reached -- UNEXPECTED\n");
                              while (true)
                                  __asm__ volatile("hlt");
                          })
                          .set_name("boot")
                          .build();
    if (boot_task != nullptr) {
        Scheduler::run_first(boot_task);
    }

    cinux::lib::kprintf("[BOOT] All tasks exited, entering idle loop.\n");
    while (true) {
        Scheduler::yield();
        __asm__ volatile("hlt");
    }
}
