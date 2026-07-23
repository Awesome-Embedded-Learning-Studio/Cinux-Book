/**
 * @file kernel/test/main_test.cpp
 * @brief Big kernel test entry point
 *
 * Replaces the production kernel_main with a test harness that initializes
 * GDT/IDT, runs the GDT/IDT test suite, and exits via QEMU isa-debug-exit.
 *
 * Exit codes:
 *   0 = all tests passed (QEMU exits with code 1 via isa-debug-exit)
 *   1 = some tests failed (QEMU exits with code 3 via isa-debug-exit)
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/arch/x86_64/extable.hpp"  // F-EXTABLE: sort_extable before tests
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/msr.hpp"     // F-VERIFY M3-2: read_msr (AP-side mechanism readback)
#include "kernel/arch/x86_64/paging.hpp"  // F9: enable_smep_smap
#include "kernel/arch/x86_64/smp.hpp"     // F5-M6: kLapicTimerVector
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/acpi/acpi.hpp"  // F-VERIFY M3-1: real acpi::init (firmware SMP topology)
#include "kernel/drivers/ahci/ahci.hpp"  // F10-M1 batch 6: ext2 mount
#include "kernel/drivers/ahci/ahci_block_device.hpp"  // F10-M1 batch 6: ext2 mount
#include "kernel/drivers/apic/local_apic.hpp"         // F5-M6: g_lapic (e1000 poll timer)
#include "kernel/drivers/pci/pci.hpp"                 // F10-M1 batch 6: PCI->AHCI for ext2
#include "kernel/fs/ext2.hpp"                         // F10-M1 batch 6: ext2 mount
#include "kernel/fs/vfs_mount.hpp"                    // F10-M1 batch 6: VFS mount
#include "kernel/lib/kallsyms.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/not_null.hpp"  // F10-M1 batch 6: NotNull<Task*>
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/percpu.hpp"          // F-VERIFY M3-2: kMaxCpus (AP result slots)
#include "kernel/proc/pid.hpp"             // F10-M1 batch 6: g_pid_alloc
#include "kernel/proc/process.hpp"         // F10-M1 batch 6: fork()
#include "kernel/proc/scheduler.hpp"       // F10-M1 batch 6: run_first/add_task
#include "kernel/proc/user_launch.hpp"     // F10-M1 batch 6: launch_user_program
#include "kernel/syscall/sys_waitpid.hpp"  // F10-M1 batch 6: sys_waitpid (WNOHANG poll)

extern "C" {
void run_gdt_idt_tests();
void run_pic_pit_tests();
void run_acpi_tests();
void run_apic_tests();
void run_hpet_tests();  // F5-M4: HPET high-res timer + RTC wall clock
void run_rtc_tests();   // F5-M4: CMOS RTC wall clock
void run_video_tests();
void run_keyboard_tests();
void run_pmm_tests();
void run_buddy_tests();
void run_slab_tests();
void run_kmalloc_tests();
void run_vmm_tests();
void run_address_space_tests();
void run_scheduler_tests();
void run_sync_tests();
void run_futex_tests();
void run_usermode_tests();
void run_syscall_tests();
void run_shell_tests();
void run_ahci_tests();
void run_ramdisk_tests();
void run_vfs_syscall_tests();
void run_ext2_tests();
void run_devfs_tests();
void run_pty_device_tests();
void run_procfs_tests();
void run_ahci_write_tests();
void run_ahci_block_device_tests();
void run_ext2_allocator_tests();
void run_ext2_ops_tests();
void run_ext2_inode_ops_tests();
void run_syscall_ext2_tests();
void run_shell_write_tests();
void run_cwd_stat_tests();
void run_shared_resources_tests();
void run_clone_tests();
void run_sync_concurrent_tests();
void run_canvas_tests();
void run_mouse_event_tests();
void run_window_tests();
void run_window_manager_tests();
void run_gui_integration_tests();
void run_bitmap_icon_tests();
void run_desktop_tests();
void run_terminal_tests();
void run_gui_swraster_tests();
void run_gui_region_tests();
void run_gui_dirty_tests();
void run_pipe_tests();
void run_sys_pipe_tests();
void run_terminal_shell_tests();
void run_fork_exec_tests();
void run_process_group_tests();
void run_multi_terminal_tests();
void run_kprintf_format_tests();
void run_concurrent_ring_buffer_tests();
void run_klog_tests();
void run_sys_dmesg_tests();
void run_dma_buffer_tests();
void run_dma_pool_tests();
void run_prdt_builder_tests();
void run_block_device_tests();
void run_vma_tests();
void run_mmap_tests();
void run_brk_tests();
void run_signal_tests();
void run_tls_tests();
void run_page_cache_tests();
void run_file_mmap_tests();
void run_kallsyms_tests();
void run_backtrace_tests();
void run_memory_stats_tests();
void run_user_ptr_tests();
void run_pmm_mapcount_tests();
#ifdef CINUX_USB
void run_xhci_tests();
#endif
void run_aslr_tests();   // F9 batch 8: ASLR offset helpers
void run_creds_tests();  // F9 batch 9: process credentials
#ifdef CINUX_NET
void run_e1000_tests();
void run_net_tests();  // F7 L1: loopback L3 stack (ping 127.0.0.1, deterministic)
#endif
}

extern "C" void net_timer_stub();  // F5-M6: e1000 RX-poll LAPIC timer ISR (interrupts.S)

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

// ============================================================
// musl ring-3 smoke (F10-M1 batch 6 static /hello + F10-M2 dynamic /hello-dyn)
//
// The unit-test suites above run single-threaded with no real dispatch loop.
// After they finish, this optional phase (CINUX_MUSL_HELLO_SMOKE) enters the
// real scheduler: a worker task forks, the child execves /hello (the musl
// static binary from tools/musl/), which exercises the batch-3 initial stack
// (auxv) and the batch-4 syscalls (arch_prctl TLS, writev printf output,
// exit_group) end-to-end. The parent waitpids and treats exit_status==0 as the
// pass signal. The worker then writes the combined exit code (unit failures OR
// hello != 0) to the isa-debug-exit device, terminating QEMU.
// ============================================================
// The harness compiles when EITHER smoke flag is on; the static /hello and
// dynamic /hello-dyn phases are gated independently inside, so each can run
// alone (CINUX_MUSL_HELLO_SMOKE / CINUX_MUSL_DYN_SMOKE).
#if defined(CINUX_MUSL_HELLO_SMOKE) || defined(CINUX_MUSL_DYN_SMOKE)
static int g_unit_test_failures = 0;

static void musl_hello_smoke_entry() {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr) {
        cinux::lib::kprintf("[F10-M1] smoke: no current task\n");
        __asm__ volatile("outl %0, $0xf4" : : "a"(1));
        while (1)
            __asm__ volatile("cli; hlt");
    }
    task->children = nullptr;

    // Mount the ext2 disk (AHCI port 1) into the global VFS so execve can
    // resolve /hello.  The harness keeps no global AHCI/ext2 (each ext2 test
    // does its own setup_ext2), so replicate that: PCI -> AHCI -> port-1 block
    // device -> Ext2 mount.  Objects are static so they outlive the worker.
    static cinux::drivers::pci::PCI*              pci     = nullptr;
    static cinux::drivers::ahci::AHCI*            ahci    = nullptr;
    static cinux::drivers::ahci::AHCIBlockDevice* blk_dev = nullptr;
    static cinux::fs::Ext2*                       ext2    = nullptr;
    if (ext2 == nullptr) {
        pci = new cinux::drivers::pci::PCI();
        pci->init();
        cinux::drivers::pci::PCIDevice ahci_dev{};
        if (pci->find_ahci(ahci_dev)) {
            ahci = new cinux::drivers::ahci::AHCI();
            ahci->init(ahci_dev);
            auto blk = cinux::drivers::ahci::AHCIBlockDevice::create(*ahci, 1);
            if (blk.ok()) {
                blk_dev = new cinux::drivers::ahci::AHCIBlockDevice(std::move(blk.value()));
            }
        }
        ext2 = new cinux::fs::Ext2(blk_dev);
        (void)ext2->mount();
    }
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", ext2);
    cinux::lib::kprintf("[F10-M1] ext2 mounted at / for smoke (mounted=%d, blk=%d)\n",
                        ext2->is_mounted() ? 1 : 0, blk_dev != nullptr ? 1 : 0);

#    ifdef CINUX_MUSL_HELLO_SMOKE
    // P3 ring-3 stress: run the musl /hello fork+execve+waitpid path repeatedly
    // (not once) to flush out intermittent accessor/CoW/cleartid/futex races --
    // the -smp2 shell "multiple /hello" saga showed these only fire on repeat.
    constexpr int kHelloIters = 20;
    int           hello_pass  = 0;
    int           hello_fail  = 0;
    cinux::lib::kprintf("[F10-M1] musl hello ring-3 smoke: %d iterations\n", kHelloIters);
    for (int hi = 0; hi < kHelloIters; ++hi) {
        int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
        if (child_pid == 0) {
            // Child: the worker is a kernel thread with no user address space,
            // so fork produced a child without one too.  Install a fresh AS
            // (mirroring the non-GUI /bin/sh launch) before execve.
            auto* child        = cinux::proc::Scheduler::current();
            child->addr_space  = new cinux::mm::AddressSpace();
            const char* argv[] = {"/hello", nullptr};
            const char* envp[] = {nullptr};
            cinux::proc::launch_user_program("/hello", argv, envp);
            cinux::proc::Scheduler::exit_current();  // unreachable
        }

        // Parent: poll for the child's exit (WNOHANG) yielding between checks.
        int     status   = -1;
        int64_t reap_ret = 0;
        for (int spins = 0; spins < 50'000'000; ++spins) {
            // Kernel inner waitpid (writes a kernel status) directly: smoke_entry
            // is a kernel task with no user AS, so sys_waitpid's put_user would
            // reject the kernel &status with -EFAULT.
            int                        kstatus = 0;
            cinux::proc::WaitpidResult wr =
                cinux::proc::waitpid(child_pid, &kstatus, 1, cinux::proc::g_pid_alloc);
            if (wr == cinux::proc::WaitpidResult::Ok) {
                status   = kstatus;
                reap_ret = child_pid;
                break;
            }
            if (wr != cinux::proc::WaitpidResult::NotExited) {
                reap_ret = static_cast<int64_t>(wr);  // error (<0)
                break;
            }
            cinux::proc::Scheduler::yield();
        }
        if (reap_ret > 0 && status == 0) {
            ++hello_pass;
        } else {
            ++hello_fail;
            cinux::lib::kprintf("[F10-M1] smoke: hello iter %d FAIL (status=%d reap=%lld)\n", hi,
                                status, static_cast<long long>(reap_ret));
        }
    }
    bool hello_ok = (hello_fail == 0);
    cinux::lib::kprintf("[F10-M1] smoke: hello %d/%d iters PASS -> %s\n", hello_pass, kHelloIters,
                        hello_ok ? "PASS" : "FAIL");
#    else
    bool hello_ok = true;  // static phase compiled out (only CINUX_MUSL_DYN_SMOKE on)
#    endif

#    ifdef CINUX_MUSL_DYN_SMOKE
    // F10-M2: dynamic musl hello -- fork+execve /hello-dyn exercises the kernel's
    // PT_INTERP / interpreter-load path end-to-end (interp mapped at
    // USER_INTERP_BASE, musl ldso relocates the main program, jumps to AT_ENTRY,
    // write() goes through libc.so). Same fork+execve+waitpid shape as the static
    // phase; fewer iters since each exec also loads + relocates the interpreter.
    constexpr int kDynIters = 5;
    int           dyn_pass  = 0;
    int           dyn_fail  = 0;
    cinux::lib::kprintf("[F10-M2] musl dynamic hello ring-3 smoke: %d iterations\n", kDynIters);
    for (int di = 0; di < kDynIters; ++di) {
        int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
        if (child_pid == 0) {
            auto* child        = cinux::proc::Scheduler::current();
            child->addr_space  = new cinux::mm::AddressSpace();
            const char* argv[] = {"/hello-dyn", nullptr};
            const char* envp[] = {nullptr};
            cinux::proc::launch_user_program("/hello-dyn", argv, envp);
            cinux::proc::Scheduler::exit_current();  // unreachable
        }

        int     status   = -1;
        int64_t reap_ret = 0;
        for (int spins = 0; spins < 50'000'000; ++spins) {
            int                        kstatus = 0;
            cinux::proc::WaitpidResult wr =
                cinux::proc::waitpid(child_pid, &kstatus, 1, cinux::proc::g_pid_alloc);
            if (wr == cinux::proc::WaitpidResult::Ok) {
                status   = kstatus;
                reap_ret = child_pid;
                break;
            }
            if (wr != cinux::proc::WaitpidResult::NotExited) {
                reap_ret = static_cast<int64_t>(wr);  // error (<0)
                break;
            }
            cinux::proc::Scheduler::yield();
        }
        if (reap_ret > 0 && status == 0) {
            ++dyn_pass;
        } else {
            ++dyn_fail;
            cinux::lib::kprintf("[F10-M2] smoke: hello-dyn iter %d FAIL (status=%d reap=%lld)\n",
                                di, status, static_cast<long long>(reap_ret));
        }
    }
    bool dyn_ok = (dyn_fail == 0);
    cinux::lib::kprintf("[F10-M2] smoke: hello-dyn %d/%d iters PASS -> %s\n", dyn_pass, kDynIters,
                        dyn_ok ? "PASS" : "FAIL");
#    else
    bool dyn_ok = true;  // dynamic phase compiled out
#    endif

    // F-VERIFY M5-2 forktest stays disabled (cross-core CoW #DF, separate bug;
    // see note above). Re-enable once fork/CoW cross-core #DF is fixed.
    bool forktest_ok = true;

    int exit_code = (g_unit_test_failures > 0 || !hello_ok || !dyn_ok || !forktest_ok) ? 1 : 0;
    __asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));
    while (1)
        __asm__ volatile("cli; hlt");
}
#endif  // CINUX_MUSL_HELLO_SMOKE || CINUX_MUSL_DYN_SMOKE

// ============================================================
// F-VERIFY M3-2: AP wake + AP-side mechanism readback
// ============================================================
// Runs ON THE AP (called from ap_main's test-mode branch, after the AP signals
// online).  Reads this AP's CR4/EFER/LSTAR/STAR/SFMASK into its result slot so
// the BSP can assert AP-side CPU-config parity.  Writes magic LAST (x86 TSO) so
// the BSP polling magic sees a complete slot.  Return value tells ap_main what
// to do AFTER the readback: true = enter the production scheduler (smoke on ->
// the AP picks up forktest children for cross-core CoW stress, M5-2b); false =
// halt (suite-only -smp gate, no scheduler).
static bool ap_test_selfcheck(uint32_t cpu_id) {
    cinux::arch::ApSelfcheckResult& r = cinux::arch::g_ap_selfcheck_results[cpu_id];
    uint64_t                        cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    r.cpu_id = cpu_id;
    r.cr4    = cr4;
    r.efer   = cinux::arch::read_msr(0xC0000080);
    r.lstar  = cinux::arch::read_msr(0xC0000082);
    r.star   = cinux::arch::read_msr(0xC0000081);
    r.sfmask = cinux::arch::read_msr(0xC0000084);
    r.magic  = cinux::arch::kApSelfcheckMagic;
#if defined(CINUX_MUSL_HELLO_SMOKE) || defined(CINUX_MUSL_DYN_SMOKE)
    // Smoke will run the scheduler -- let this AP participate (cross-core CoW).
    return true;
#else
    // Suite-only build: no scheduler, halt to avoid an is_initialized spin hang.
    return false;
#endif
}

// Wake APs via boot_aps and assert AP-side CPU-config readback on the BSP.
// Returns false on any SMP assertion failure (OR'd into exit_code so a regression
// fails the suite).  No-op (returns true) when cpu_count<=1 -- the SMP path only
// engages under -smp 2, where M3-1's acpi::init detected >1 CPU.  This is what
// turns run-kernel-test-smp from a BSP-only no-op into a real AP-wake gate.
static bool run_smp_ap_wake_test() {
    const auto&    info     = cinux::drivers::acpi::g_acpi_info;
    const uint32_t ap_count = info.cpu_count > 0 ? info.cpu_count - 1 : 0;
    if (ap_count == 0) {
        cinux::lib::kprintf("[F-VERIFY M3-2] single-CPU: AP wake test skipped\n");
        return true;
    }

    // boot_aps sends INIT-SIPI-SIPI via LAPIC IPI; the test kernel must init the
    // LAPIC (IPI-capable) first.  g_pmm is already up (suites ran above).
    cinux::drivers::apic::g_lapic.init(0xFEE00000);
    cinux::drivers::apic::g_lapic.enable(0xFF);

    cinux::arch::g_ap_test_selfcheck_fn = ap_test_selfcheck;
    cinux::lib::kprintf("[F-VERIFY M3-2] booting %u AP(s) + readback...\n", ap_count);
    cinux::arch::boot_aps();

    bool ok = true;
    for (uint32_t cpu = 1; cpu <= ap_count && cpu < cinux::proc::kMaxCpus; cpu++) {
        // Poll the AP's slot until its selfcheck completes (magic written last).
        const volatile cinux::arch::ApSelfcheckResult* slot =
            &cinux::arch::g_ap_selfcheck_results[cpu];
        for (volatile uint32_t spin = 0; spin < 100000000; spin++) {
            if (slot->magic == cinux::arch::kApSelfcheckMagic) {
                break;
            }
        }
        // Field-by-field read from the volatile slot (aggregate copy can't bind a
        // volatile source).  x86 TSO + magic-written-last => a consistent set.
        const uint32_t magic = slot->magic;
        const uint64_t cr4   = slot->cr4;
        const uint64_t efer  = slot->efer;
        const uint64_t lstar = slot->lstar;
        cinux::lib::kprintf("[F-VERIFY M3-2] AP%u: magic=0x%lx cr4=0x%lx efer=0x%lx lstar=0x%lx\n",
                            cpu, static_cast<unsigned long>(magic), static_cast<unsigned long>(cr4),
                            static_cast<unsigned long>(efer), static_cast<unsigned long>(lstar));
        if (magic != cinux::arch::kApSelfcheckMagic) {
            cinux::lib::kprintf("[F-VERIFY M3-2] FAIL AP%u: never ran selfcheck\n", cpu);
            ok = false;
            continue;
        }
        if (lstar == 0) {
            cinux::lib::kprintf("[F-VERIFY M3-2] FAIL AP%u: LSTAR==0 (syscall RIP -> #DF class)\n",
                                cpu);
            ok = false;
        }
        if (((cr4 >> 9) & 1) == 0 || ((cr4 >> 10) & 1) == 0) {
            cinux::lib::kprintf("[F-VERIFY M3-2] FAIL AP%u: CR4 OSFXSR/OSXMMEXCPT clear (0x%lx)\n",
                                cpu, static_cast<unsigned long>(cr4));
            ok = false;
        }
        if ((efer & (1ULL << 11)) == 0) {
            cinux::lib::kprintf("[F-VERIFY M3-2] FAIL AP%u: EFER.NXE clear\n", cpu);
            ok = false;
        }
    }
    cinux::lib::kprintf("[F-VERIFY M3-2] AP wake + readback: %s\n", ok ? "PASS" : "FAIL");
    cinux::arch::g_ap_test_selfcheck_fn = nullptr;  // disarm (APs already halted)
    return ok;
}

extern "C" void kernel_main() {
    // Step 1: Initialise serial port for test output
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[TEST] Big Kernel Test Suite starting...\n");

    // F-INFRA I-5: register the build-generated symbol table. Individual suites
    // (run_kallsyms_tests) override this with a fixture to test lookup logic, so
    // this mainly serves early-boot/panic backtraces before those suites run.
    cinux::lib::kallsyms_set_table(g_kallsyms_table, g_kallsyms_count);

    // Step 2: Initialise GDT (must come before IDT)
    cinux::arch::gdt_blocks[0].init();
    cinux::lib::kprintf("[TEST] GDT loaded.\n");

    // Step 3: Initialise IDT (depends on GDT selectors)
    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[TEST] IDT loaded.\n");

    // F-EXTABLE: sort the user-accessor exception table before any suite that
    // could fault through an accessor (demand-page / fork / CoW). No-op while
    // empty; mirrors production main.cpp.
    cinux::arch::sort_extable();

    // F4-M3 P1-2: anchor the BSP's GS base at its PerCpu block BEFORE any test
    // suite runs -- percpu() reads MSR_GS_BASE, so it must be set first.  This
    // also configures STAR/EFER for SYSRET; run_usermode_tests still observes them.
    cinux::arch::usermode_init();
    cinux::arch::enable_smep_smap();  // F9 batch 3/4: mirror production main (test_f9 verifies CR4)

    // F2-M7 direct-map identity probe (batch 1): the loader mapped all RAM into
    // the DIRECT_MAP_BASE window with 1 GB huge pages.  Verify it is identity by
    // comparing bytes seen through DIRECT_MAP_BASE + phys against the existing
    // KERNEL_VMA + phys window (valid for phys < 1 GB).  The 1 GB pages are
    // uniform, so a low-phys match confirms the whole window is wired correctly.
    {
        // Diagnostics: dump CR3 and the PML4 entry that should map the window.
        // PML4[272] is read via the existing higher-half mapping of phys 0x1000.
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        const volatile uint64_t* pml4 =
            reinterpret_cast<const volatile uint64_t*>(cinux::arch::KERNEL_VMA + 0x1000);
        cinux::lib::kprintf("[TEST] dmap diag: cr3=0x%lx pml4[272]=0x%lx\n", cr3, pml4[272]);

        bool dmap_ok = true;
        for (uint64_t probe : {0x200000ULL, 0x800000ULL, 0x1000000ULL}) {
            const uint8_t* a =
                reinterpret_cast<const uint8_t*>(cinux::arch::DIRECT_MAP_BASE + probe);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(cinux::arch::KERNEL_VMA + probe);
            if (a[0] != b[0] || a[7] != b[7]) {
                dmap_ok = false;
            }
        }
        cinux::lib::kprintf("[TEST] direct-map identity probe: %s\n", dmap_ok ? "OK" : "MISMATCH");
    }

    // F-VERIFY M3-1: real ACPI firmware scan -> g_acpi_info (cpu_count + apic_ids).
    // The test kernel previously only tested the MADT PARSER on synthetic tables
    // (run_acpi_tests), never the real firmware scan -- so g_acpi_info.cpu_count
    // stayed 0 and boot_aps() bailed as "single CPU".  This real scan is the
    // prerequisite for M3-2 (waking APs).  Under run-kernel-test-smp (-smp 2) it
    // reports cpu_count>=2; under single-CPU run-kernel-test it reports 1.
    cinux::drivers::acpi::init();
    {
        const auto& info = cinux::drivers::acpi::g_acpi_info;
        cinux::lib::kprintf("[F-VERIFY M3-1] SMP topology: cpu_count=%u",
                            static_cast<unsigned>(info.cpu_count));
        for (uint32_t i = 0; i < info.cpu_count && i < 8; i++) {
            cinux::lib::kprintf(" apic_id[%u]=%u", i, info.cpu_apic_ids[i]);
        }
        cinux::lib::kprintf("\n");
        if (info.cpu_count > 1) {
            cinux::lib::kprintf(
                "[F-VERIFY M3-1] SMP mode (%u CPUs) -- M3-2 AP-wake tests engage here\n",
                static_cast<unsigned>(info.cpu_count));
        }
    }

    // kprintf format tests run early — only need serial + kprintf
    run_kprintf_format_tests();

    // FO observability (batch 1a): KALLSYMS address->symbol lookup logic.  The
    // production kernel feeds a real nm-generated table at boot; the test
    // suite injects a fixture inside run_kallsyms_tests().
    run_kallsyms_tests();

    // Step 4: Run test suites (hardware only)
    run_gdt_idt_tests();
    run_pic_pit_tests();
    run_keyboard_tests();
    run_acpi_tests();
    run_apic_tests();

    // PMM tests: initialise with real BootInfo, then run tests
    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    cinux::mm::g_pmm.init(*boot_info);
    run_pmm_tests();
    run_buddy_tests();

    // VMM tests: initialise VMM after PMM, then run tests
    cinux::mm::g_vmm.init();
    run_vmm_tests();

    // F5-M4: HPET high-res timer + RTC wall clock.  HPET needs VMM (it maps its
    // MMIO window via g_vmm) and ACPI (find_table); both are up by here.  B1
    // covers the table parse; B2 adds the driver mechanism tests.
    run_hpet_tests();
    // RTC is pure port I/O (0x70/0x71); no VMM/ACPI dependency.
    run_rtc_tests();

    // FO batch 2: backtrace (needs VMM for translate-based safe walk).
    run_backtrace_tests();

    // Slab tests (F2-M7b): initialise after VMM (slab maps pages) and PMM.
    cinux::mm::g_slab.init(cinux::arch::KMEM_SLAB_BASE, cinux::arch::KMEM_SLAB_SIZE);
    cinux::mm::init_dedicated_caches();  // F2-M7b: task / vma / cached_page caches
    run_slab_tests();
    run_kmalloc_tests();

    // Video tests require VMM (framebuffer maps via map_2mb)
    run_video_tests();

    // Page cache (F2-M4): advisory 10% ceiling; eviction deferred.  Needs the
    // slab -- get_page() allocates CachedPage nodes via new (-> kmalloc).
    cinux::mm::g_page_cache.init(cinux::mm::g_pmm.free_page_count() / 10);

    run_pipe_tests();
    run_sys_pipe_tests();
    run_canvas_tests();
    run_mouse_event_tests();
    run_window_tests();
    run_window_manager_tests();
    run_gui_integration_tests();
    run_bitmap_icon_tests();
    run_desktop_tests();
    run_terminal_tests();
#ifdef CINUX_GUI
    run_terminal_shell_tests();
    // F13 cinux::gui §4a: SwRaster primitive unit tests.
    run_gui_swraster_tests();
    // F13 cinux::gui §4b: region algebra unit tests.
    run_gui_region_tests();
    // F13 cinux::gui §4c: dirty-region + flush path tests.
    run_gui_dirty_tests();
#endif
    cinux::mm::AddressSpace::init_kernel();
    run_address_space_tests();
    run_vma_tests();
    run_mmap_tests();
    run_brk_tests();
    run_signal_tests();
    run_tls_tests();
    run_page_cache_tests();
    run_file_mmap_tests();

    // FO batch 4: memory diagnostics dump (all MM subsystems are up by here).
    run_memory_stats_tests();

    run_scheduler_tests();
    run_sync_tests();
    run_futex_tests();
    run_sync_concurrent_tests();
    run_concurrent_ring_buffer_tests();
    run_klog_tests();
    run_sys_dmesg_tests();
    run_user_ptr_tests();
    run_pmm_mapcount_tests();

    // DMA tests (M3): DmaBuffer value type (M3-1) + DmaPool allocator (M3-2)
    run_dma_buffer_tests();
    run_dma_pool_tests();
    run_prdt_builder_tests();

    // Block device tests (M4): IBlockDevice interface + RAMBlockDevice stub (M4-1)
    run_block_device_tests();

    run_aslr_tests();   // F9 batch 8: ASLR offset helpers (page-align / range / vary)
    run_creds_tests();  // F9 batch 9: process credentials
    run_usermode_tests();

    cinux::arch::syscall_init();
    run_syscall_tests();

    run_fork_exec_tests();
    run_process_group_tests();
#ifdef CINUX_GUI
    // Multi-terminal tests (035): multiple concurrent terminals with
    // independent pipes, destructor cleanup, WM iteration, tick callback
    run_multi_terminal_tests();
#endif
    // Shell tests (024): verifies kernel-side infrastructure for user shell
    run_shell_tests();

    // AHCI tests (025): requires PMM and VMM for BAR5 mapping and DMA buffers
    run_ahci_tests();

#ifdef CINUX_USB
    // xHCI tests (F5-M5): PCI find + BAR0 map + reset.  Skips (passes) when no
    // qemu-xhci is present (default config); exercises real bring-up under the
    // run-kernel-test-xhci target.
    run_xhci_tests();
#endif

#ifdef CINUX_NET
    // Arm a LAPIC periodic timer (vector 0x30) so e1000 RX poll can sti+hlt
    // between polls: hlt lets QEMU's device-model main loop run and pull SLIRP
    // replies into the e1000 ring, and the timer IRQ wakes the CPU.  The test
    // kernel runs IF=0 everywhere else, so without this the main loop never
    // advances during a busy poll and GPRC stays 0.  Handler is net_timer_stub
    // (no-op + direct LAPIC EOI, registered into the shared IDT).  IF keeps the
    // timer masked outside e1000's poll, so other suites are undisturbed.
    cinux::drivers::apic::g_lapic.init(0xFEE00000);  // QEMU x86 LAPIC MMIO (fixed)
    cinux::drivers::apic::g_lapic.enable(0xFF);      // SVR: spurious=0xFF + APIC enable
    cinux::arch::g_idt.set_handler(
        static_cast<cinux::arch::ExceptionVector>(cinux::arch::kLapicTimerVector), net_timer_stub,
        cinux::arch::GDT_KERNEL_CODE,
        cinux::arch::make_idt_attr(cinux::arch::IDTPrivilege::Kernel,
                                   cinux::arch::IDTGateType::Interrupt),
        0);
    cinux::drivers::apic::g_lapic.setup_periodic_timer(cinux::arch::kLapicTimerVector, 0x3,
                                                       200'000);  // /16, ~300 Hz

    // e1000 tests (F5-M6 批a): PCI find + BAR0 map + reset + EEPROM MAC.  Skips
    // (passes) when no e1000 is present; exercises real bring-up under
    // run-kernel-test (-device e1000) / run-kernel-test-net.
    run_e1000_tests();

    // F7 L1: the L3 stack on loopback -- ARP/IPv4/ICMP ping 127.0.0.1.  No SLIRP
    // timing (loopback is synchronous), so this runs unconditionally under
    // CINUX_NET, before the LAPIC-timer-dependent e1000 RX above is any concern.
    run_net_tests();
#endif

    // Ramdisk tests (026): verifies ustar parsing of embedded initrd
    run_ramdisk_tests();

    // VFS syscall integration tests (027): sys_open/read/write/close via VFS
    run_vfs_syscall_tests();

    // Ext2 filesystem tests (028): mount, lookup, read, readdir, VFS integration
    run_ext2_tests();

    // DevFS tests (F6-M3): /dev/null, /dev/zero, /dev/console, readdir, stat
    run_devfs_tests();

    // ProcFS tests (F6-M2): /proc root readdir, /proc/<pid> lookup + stat,
    // stat/cmdline pseudo-files.
    run_procfs_tests();

    // PTY device tests (F10-M3 Phase 2): alloc, master<->slave round-trip,
    // echo, termios ioctl, TIOCGPTN.
    run_pty_device_tests();

    // AHCI write + ext2 write_block tests (028b): write round-trip, write_block
    run_ahci_write_tests();

    // AHCI block device adapter (M4-2): IBlockDevice over real AHCI hardware
    run_ahci_block_device_tests();

    // Ext2 allocator tests (028b): alloc_block, free_block, alloc_inode, free_inode
    run_ext2_allocator_tests();

    // Ext2 write/create/mkdir/unlink tests (028b)
    run_ext2_ops_tests();

    // Ext2 InodeOps virtual class tests (028b)
    run_ext2_inode_ops_tests();

    // Syscall ext2 integration tests (028b): sys_creat/mkdir/unlink/rmdir
    run_syscall_ext2_tests();

    // Shell write command tests (028b): touch/mkdir/rm/rmdir/echo redirect
    run_shell_write_tests();

    // CWD/stat tests (028c): chdir/getcwd/stat/fstat/path canonicalize
    run_cwd_stat_tests();
    run_shared_resources_tests();
    run_clone_tests();

    // Step 5: Report and exit
    // F-VERIFY M3-2: AP wake + AP-side readback (-smp 2 only; no-op single-CPU).
    bool smp_ok    = run_smp_ap_wake_test();
    int  exit_code = (test::get_total_failed() > 0 || !smp_ok) ? 1 : 0;

    if (exit_code != 0) {
        cinux::lib::kprintf("\n[TEST] TESTS FAILED (exit code %d)\n", exit_code);
    } else {
        cinux::lib::kprintf("\n[TEST] ALL TESTS PASSED (exit code %d)\n", exit_code);
    }

#if defined(CINUX_MUSL_HELLO_SMOKE) || defined(CINUX_MUSL_DYN_SMOKE)
    // F10-M1 batch 6 / F10-M2 batch 3: enter the real scheduler and run the musl
    // The worker task signals QEMU exit itself (isa-debug-exit), so control
    // does not return here.  CI builds without the flag take the normal path.
    cinux::lib::kprintf("\n[F10-M1] entering scheduler for musl hello ring-3 smoke\n");
    g_unit_test_failures = test::get_total_failed();
    cinux::proc::Scheduler::init();  // pristine run queue for the smoke (tests re-init it)
    auto* hello_worker = cinux::proc::TaskBuilder()
                             .set_entry(musl_hello_smoke_entry)
                             .set_name("hello_smoke")
                             .build();
    auto* boot_ctx =
        cinux::proc::TaskBuilder().set_entry(musl_hello_smoke_entry).set_name("boot_ctx").build();
    if (hello_worker != nullptr && boot_ctx != nullptr) {
        // wake_ap=false: do NOT IPI an idle AP for the initial worker -- tilt the
        // race toward the BSP running the smoke. (Not a hard guarantee: an AP in
        // sti;hlt can still be pulled out by a LAPIC-timer tick and steal the
        // worker. The park below handles that case.)
        cinux::proc::Scheduler::add_task(cinux::lib::NotNull<cinux::proc::Task*>{hello_worker},
                                         /*wake_ap=*/false);
        cinux::proc::Scheduler::run_first(cinux::lib::NotNull<cinux::proc::Task*>{boot_ctx});
        // run_first() returns ONLY if the run queue was empty, which means an AP
        // seized hello_worker first (a task is removed from the queue exactly
        // once by pick_next; if the BSP did not get it, the AP did). The smoke
        // is therefore running on the AP and will exit QEMU itself via
        // isa-debug-exit. The BSP must NOT outl here -- that would kill the AP
        // mid-smoke (the old false-red/green). Park with interrupts off until
        // the AP's outl ends QEMU.
        cinux::lib::kprintf("[F10-M1] smoke seized by AP; BSP parking until QEMU exit\n");
        while (true) {
            __asm__ volatile("cli; hlt");
        }
    } else {
        cinux::lib::kprintf("[F10-M1] smoke task alloc failed\n");
        exit_code = 1;
    }
#endif

    // Exit via QEMU isa-debug-exit device (port 0xf4)
    __asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));

    // Fallback halt if isa-debug-exit is not available
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
