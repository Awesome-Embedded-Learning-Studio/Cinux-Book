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
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/lib/kallsyms.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_cache.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/mm/vmm.hpp"

extern "C" {
void run_gdt_idt_tests();
void run_pic_pit_tests();
void run_acpi_tests();
void run_apic_tests();
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
}

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

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

    // F4-M3 P1-2: anchor the BSP's GS base at its PerCpu block BEFORE any test
    // suite runs -- percpu() reads MSR_GS_BASE, so it must be set first.  This
    // also configures STAR/EFER for SYSRET; run_usermode_tests still observes them.
    cinux::arch::usermode_init();

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

    // Ramdisk tests (026): verifies ustar parsing of embedded initrd
    run_ramdisk_tests();

    // VFS syscall integration tests (027): sys_open/read/write/close via VFS
    run_vfs_syscall_tests();

    // Ext2 filesystem tests (028): mount, lookup, read, readdir, VFS integration
    run_ext2_tests();

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
    int exit_code = (test::get_total_failed() > 0) ? 1 : 0;

    if (exit_code != 0) {
        cinux::lib::kprintf("\n[TEST] TESTS FAILED (exit code %d)\n", exit_code);
    } else {
        cinux::lib::kprintf("\n[TEST] ALL TESTS PASSED (exit code %d)\n", exit_code);
    }

    // Exit via QEMU isa-debug-exit device (port 0xf4)
    __asm__ volatile("outl %0, $0xf4" : : "a"(exit_code));

    // Fallback halt if isa-debug-exit is not available
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
