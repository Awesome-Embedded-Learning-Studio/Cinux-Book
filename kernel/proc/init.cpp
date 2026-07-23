#include "kernel/proc/init.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/procfs/procfs.hpp"
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

// usb_init.hpp is unconditional: when CINUX_USB is off, usb_stub.cpp supplies
// empty usb::init()/poll_input() (§14 file gate), so this TU needs no #ifdef.
#include "kernel/drivers/usb/usb_init.hpp"
#include "kernel/proc/userspace.hpp"  // launch_userspace: GUI/non-GUI impl chosen by CMake (§14)

namespace cinux::proc {

void kernel_init_thread() {
    auto* self = Scheduler::current();

    // B3b (GCC self-host): this kthread becomes PID 1, the real init -- the
    // Linux kernel_init model.  TaskBuilder leaves kernel threads at pid=0
    // (they never touch g_pid_alloc), and no fork() precedes this point in
    // boot, so the first alloc() returns 1.  execve preserves pid, so busybox
    // init -- execved below -- inherits PID1 and reaps orphaned children: the
    // hard prerequisite for the cc1/as/ld fork chains that B4 GCC runs.  (The
    // old handoff's "reorder start_poll_driver" note was a misread: net_poll is
    // also pid=0; only fork() draws from g_pid_alloc.)
    if (self != nullptr) {
        self->pid          = g_pid_alloc.alloc();
        self->tgid         = self->pid;
        self->group_leader = self;
    }

    cinux::lib::kprintf("[INIT] kernel_init started tid=%lu pid=%d\n", self ? self->tid : 0,
                        self ? self->pid : 0);

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

    // DevFS: /dev/null, /dev/zero, /dev/console (F6-M3).
    cinux::fs::devfs::init();

    // ProcFS: /proc process introspection -- root lists live PIDs,
    // /proc/<pid>/{stat,cmdline} pseudo-files (F6-M2).
    cinux::fs::procfs::init();

    // B3b: arm USB input (xHCI + HID boot mouse + keyboard) BEFORE
    // launch_userspace -- the non-GUI launch_userspace execves /sbin/init and
    // never returns, so anything placed after it never runs.  Interrupt-driven
    // once armed; graceful no-op if no xHCI controller is present or USB is
    // compiled out (usb_stub.cpp is linked).  (The GUI build's desktop_launch
    // spawns a separate gui_worker, so USB ordering there is unchanged.)
    cinux::drivers::usb::init();

    // Bring up userspace.  GUI build: desktop + gui_worker thread
    // (kernel/gui/desktop_launch.cpp).  Non-GUI build: execve /sbin/init as
    // PID1 (kernel/proc/shell_launch.cpp) -- busybox init, which forks /
    // respawns /bin/sh per /etc/inittab.  §14: one interface, two impl files,
    // CMake selects which to link -- no #ifdef here.
    launch_userspace();

    // Unreachable in the non-GUI build: launch_userspace jumps to user mode
    // (busybox init).  Kept as a safety net for any future launch_userspace
    // variant that returns.
    Scheduler::exit_current();
}

}  // namespace cinux::proc
