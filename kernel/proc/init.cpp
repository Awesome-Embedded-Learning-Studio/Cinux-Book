#include "kernel/proc/init.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/user_launch.hpp"

#ifdef CINUX_GUI
#    include "kernel/gui/gui_init.hpp"
#    include "kernel/gui/host_cinux.hpp"
#    include "third_party/Cinux-GUI/core/pump.hpp"
#endif
#ifdef CINUX_USB
#    include "kernel/drivers/usb/usb_init.hpp"
#    include "kernel/drivers/usb/xhci_controller.hpp"  // poll_events() in gui_worker
#endif

namespace cinux::proc {

#ifdef CINUX_GUI
namespace {

void gui_worker_thread() {
    cinux::lib::kprintf("[GUI] Worker thread started\n");
    while (true) {
        // Drive the desktop through the cinux::gui Host ABI table (F13 §3b): input,
        // time and spawn all go via host->core.* / host->desktop->*. The same
        // pump body runs unchanged on a different host fill.
        cinux::gui::pump(&cinux::gui::cinux_host());
#if defined(CINUX_USB)
        // Service the xHCI event ring each frame.  On QEMU under nested-KVM the
        // interrupter's IMAN.IE does not latch, so the MSI-X transfer-complete
        // interrupt is not reliably delivered to the CPU; polling the ring here
        // is the production event-service path (mouse/keyboard Transfer Events
        // are dequeued -> on_transfer_complete -> decode + inject + re-arm).  It
        // is cheap when the mouse is idle (dequeue finds the ring empty).  The
        // MSI-X setup in XHCIController::start() is left in place -- it is
        // spec-correct and works on real hardware / a future QEMU.
        if (cinux::drivers::usb::XHCIController::has_controller()) {
            cinux::drivers::usb::XHCIController::instance().poll_events();
        }
#endif
        Scheduler::yield();
    }
}

}  // anonymous namespace
#endif

void kernel_init_thread() {
    auto* self = Scheduler::current();
    cinux::lib::kprintf("[INIT] kernel_init started tid=%lu\n", self ? self->tid : 0);

    cinux::lib::kprintf("[INIT] ===== Milestone 028: ext2 Filesystem =====\n");
    static auto blk_dev =
        cinux::drivers::ahci::AHCIBlockDevice::create(cinux::drivers::ahci::AHCI::instance(), 1);
    static cinux::fs::Ext2 ext2(blk_dev.ok() ? &blk_dev.value() : nullptr);
    auto                   mount_result = ext2.mount();
    if (!mount_result.ok()) {
        cinux::lib::kprintf("[INIT] ext2 mount failed: %s\n",
                            cinux::lib::error_string(mount_result.error()));
    }

    cinux::lib::kprintf("[INIT] ===== Milestone 027: VFS =====\n");
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", &ext2);
    cinux::lib::kprintf("[VFS] ext2 mounted at /\n");

#ifdef CINUX_GUI
    // Pipe creation is now handled per-terminal inside create_shell_terminal().
    // No global pipe setup needed here -- each Shell desktop icon click
    // dynamically creates its own pipe pair, forks a shell, and binds them.

    cinux::lib::kprintf("[INIT] ===== Milestone 035: Multi-Terminal =====\n");

    // Start the GUI: mouse init, desktop icons, PIT tick callback
    cinux::gui::gui_start();

    // Launch GUI worker thread to handle deferred work (fork/execve)
    // outside of PIT interrupt context
    auto* gui_task = TaskBuilder().set_entry(gui_worker_thread).set_name("gui_worker").build();
    if (gui_task != nullptr) {
        Scheduler::add_task(gui_task);
        cinux::lib::kprintf("[INIT] GUI worker thread launched\n");
    }
#else
    // Non-GUI mode: fork and execve the shell directly.
    // Uses legacy sys_read (keyboard polling) for stdin and
    // legacy sys_write (kprintf serial+console) for stdout.
    cinux::lib::kprintf("[INIT] Launching shell (non-GUI mode)...\n");
    int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
    if (child_pid == 0) {
        __asm__ volatile("cli");

        auto* task       = cinux::proc::Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();

        const char* path   = "/bin/sh";
        const char* argv[] = {path, nullptr};
        const char* envp[] = {nullptr};

        cinux::proc::launch_user_program(path, argv, envp);
    } else if (child_pid > 0) {
        cinux::lib::kprintf("[INIT] Shell spawned pid=%d\n", child_pid);
    } else {
        cinux::lib::kprintf("[INIT] fork() failed: %d\n", child_pid);
    }
#endif

#ifdef CINUX_USB
    // Bring up USB input (xHCI + HID boot mouse + keyboard).  Runs AFTER the
    // GUI/shell is up so its synchronous enumeration does not delay the desktop
    // (gui_worker is an independent thread that keeps rendering).  Interrupt-
    // driven once armed.  Graceful no-op if no xHCI controller is present.
    cinux::drivers::usb::init();
#endif

    Scheduler::exit_current();
}

}  // namespace cinux::proc
