#include "kernel/proc/init.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/ahci/ahci_block_device.hpp"
#include "kernel/drivers/block_registry.hpp"  // F6-M1 B1b: register block devices
#include "kernel/drivers/nvme/nvme_block_device.hpp"
#include "kernel/drivers/virtio/virtio_blk.hpp"  // F5-M2 task 3: virtio_block_device()
#include "kernel/fs/devfs/devfs.hpp"
#include "libs/ext2/ext2.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/tmpfs/tmpfs.hpp"  // F6-M4: tmpfs::init (/tmp)
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/arch/x86_64/tlb.hpp"  // B3 defect C: start_tlb_drain_thread
#include "kernel/mm/diagnostics.hpp"
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
#include "kernel/net/net_init.hpp"    // F5-M2 task 2: ping() virtio-net SLIRP gate
#include "kernel/proc/userspace.hpp"  // launch_userspace: GUI/non-GUI impl chosen by CMake (§14)

namespace cinux::proc {

namespace {
// F5-M2 task 3: virtio-blk vs NVMe vs AHCI read-perf comparison.  Reads
// block (i % 64) for kIters iterations (avoiding the fixed-block cache effect)
// and reports rdtsc ticks -- READ-ONLY so it's safe on the NVMe boot disk too.
// QEMU emulates all three via the same block backend, so absolute numbers are
// QEMU-flavoured; the RELATIVE comparison is the point.
inline uint64_t rdtsc_now() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void perf_read(const char* name, cinux::drivers::IBlockDevice& dev, void* buf) {
    constexpr uint32_t kIters = 256;
    const auto         t0     = rdtsc_now();
    for (uint32_t i = 0; i < kIters; ++i) {
        static_cast<void>(dev.read_blocks(i % 64, 1, buf));
    }
    const auto t1 = rdtsc_now();
    cinux::lib::kprintf("[perf] %-10s: %u reads = %llu ticks (%llu ticks/read)\n", name, kIters,
                        static_cast<unsigned long long>(t1 - t0),
                        static_cast<unsigned long long>((t1 - t0) / kIters));
}
}  // namespace

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
    // Boot disk: prefer NVMe (perf path) if its namespace carries a valid ext2
    // fs; else fall back to AHCI.  Same kernel image runs either rootfs (qemu
    // -device nvme with rootfs on NVMe -> NVMe boot; AHCI rootfs -> AHCI boot)
    // -- runtime select, no #ifdef (§14 file-gate spirit).  Ext2(nullptr) would
    // crash in mount(), so the NVMe Ext2 is constructed only when a device exists.
    cinux::fs::Ext2*              rootfs    = nullptr;
    cinux::drivers::IBlockDevice* root_bdev = nullptr;  // F6-M1 B1b: backing dev for sys_mount
    auto*            nvme_bd = cinux::drivers::nvme::nvme_block_device();
    if (nvme_bd != nullptr) {
        static cinux::fs::Ext2 nvme_ext2(nvme_bd);
        auto                   nvme_m = nvme_ext2.mount();
        if (nvme_m.ok()) {
            rootfs    = &nvme_ext2;
            root_bdev = nvme_bd;
            cinux::lib::kprintf("[INIT] rootfs on NVMe (perf path)\n");
        } else {
            // Log why the NVMe ext2 mount failed -- otherwise this fallback is
            // silent and the boot mysteriously drops to AHCI (or #GPs on the
            // AHCI fallback when no AHCI controller exists, as in the release
            // run.sh boot: IDE boot disk + NVMe rootfs, no AHCI device).
            cinux::lib::kprintf("[INIT] NVMe ext2 mount failed: %s (try AHCI)\n",
                                cinux::lib::error_string(nvme_m.error()));
        }
    }
    if (rootfs == nullptr && cinux::drivers::ahci::AHCI::is_present()) {
        static auto ahci_blk = cinux::drivers::ahci::AHCIBlockDevice::create(
            cinux::drivers::ahci::AHCI::instance(), 1);
        static cinux::fs::Ext2 ahci_ext2(ahci_blk.ok() ? &ahci_blk.value() : nullptr);
        auto                   m = ahci_ext2.mount();
        if (!m.ok()) {
            cinux::lib::kprintf("[INIT] AHCI ext2 mount failed: %s\n",
                                cinux::lib::error_string(m.error()));
        }
        rootfs    = &ahci_ext2;
        root_bdev = ahci_blk.ok() ? &ahci_blk.value() : nullptr;
        cinux::lib::kprintf("[INIT] rootfs on AHCI\n");
    }

    cinux::lib::kprintf("[INIT] ===== Milestone 027: VFS =====\n");
    cinux::fs::vfs_mount_init();
    cinux::fs::vfs_mount_add("/", rootfs);
    cinux::lib::kprintf("[VFS] ext2 mounted at /\n");

    // F6-M1 B1b: register block devices so sys_mount -t ext2 can resolve a
    // source path (/dev/sda ...) to an IBlockDevice.  devfs::init below iterates
    // the registry to publish /dev/<name> nodes.
    if (root_bdev != nullptr) {
        cinux::drivers::BlockRegistry::register_device("sda", root_bdev);
    }
    if (auto* vblk = cinux::drivers::virtio::virtio_block_device()) {
        cinux::drivers::BlockRegistry::register_device(root_bdev ? "sdb" : "sda", vblk);
    }

    // DevFS: /dev/null, /dev/zero, /dev/console (F6-M3).
    cinux::fs::devfs::init();

    // ProcFS: /proc process introspection -- root lists live PIDs,
    // /proc/<pid>/{stat,cmdline} pseudo-files (F6-M2).
    cinux::fs::procfs::init();

    // TmpFS: /tmp writable in-memory filesystem -- where GCC / cc1 / as / ld
    // write intermediate *.o / *.s during a compile (F6-M4, GCC self-host).
    cinux::fs::tmpfs::init();

    // B3b: arm USB input (xHCI + HID boot mouse + keyboard) BEFORE
    // launch_userspace -- the non-GUI launch_userspace execves /sbin/init and
    // never returns, so anything placed after it never runs.  Interrupt-driven
    // once armed; graceful no-op if no xHCI controller is present or USB is
    // compiled out (usb_stub.cpp is linked).  (The GUI build's desktop_launch fork+execve's the
    // userspace GUI host, so USB ordering there is unchanged.)
    cinux::drivers::usb::init();

    // B1 gcc-stutter profiling: spawn the periodic memory-stats kthread.  No-op
    // when CINUX_STATS_KTHREAD=OFF (stub); prints a 1 Hz PMM/slab/PageCache/#PF
    // curve to the serial log when ON, for narrowing gcc/g++ compile-stutter.
    cinux::mm::start_stats_thread();

    // B3 defect C: spawn the TLB drain kthread.  Sets g_drain_active so CoW
    // frees defer to it (shootdown + free at IF=1, avoiding the sync-shootdown
    // deadlock two CoW-faulting CPUs would hit).  Empty stub when
    // CINUX_TLB_DRAIN=OFF (then enqueue inline-frees).  Needs the scheduler
    // (Semaphore::wait), so this production-only call sits after Scheduler::init.
    cinux::arch::start_tlb_drain_thread();

    // Bring up userspace.  GUI build: fork+execve the userspace GUI host
    // (kernel/gui/desktop_launch.cpp).  Non-GUI build: execve /sbin/init as
    // PID1 (kernel/proc/shell_launch.cpp) -- busybox init, which forks /
    // respawns /bin/sh per /etc/inittab.  §14: one interface, two impl files,
    // CMake selects which to link -- no #ifdef here.

    // F5-M2 task 2: boot-time virtio-net SLIRP ping gate (before launch_userspace
    // execves busybox init -- this stops being a kernel thread).  dev_for()
    // routes this via virtio-net.
    {
        // Uses the legacy sti/hlt pump (explicit poll): pump_yield assumes a
        // user-process syscall
        // context where yield() lets the net_poll kthread drain RX, but kernel_init
        // is a kernel thread whose yield() does not reliably hand off to net_poll
        // before the reply arrives -> ping stalls.  sti/hlt is safe HERE because
        // kernel_init has no syscall trap frame (the sys_ping #DF hazard is
        // syscall-only).  dev_for() routes this via virtio-net.
        constexpr uint16_t kPingId = 0xC1C0;
        auto r = cinux::net::ping({{10, 0, 2, 2}}, kPingId, 1, cinux::net::rx_pump_sti_hlt);
        if (r.ok() && r.value().got_reply) {
            cinux::lib::kprintf(
                "[net] PING 10.0.2.2: reply (id=0x%x seq=%u) -- "
                "virtio-net SLIRP path OK\n",
                r.value().id, r.value().seq);
        } else {
            cinux::lib::kprintf("[net] PING 10.0.2.2: no reply -- virtio-net SLIRP path FAILED\n");
        }
    }

    // F5-M2 task 3: virtio-blk vs NVMe vs AHCI read-perf comparison (boot-time
    // micro-benchmark, read-only).  virtio-blk/NVMe are the production-registered
    // devices; AHCI port 0 is the test disk (port 1 is the AHCI rootfs fallback).
    {
        static uint8_t perf_buf[4096];  // 1 block buffer
        auto*          vbd = cinux::drivers::virtio::virtio_block_device();
        if (vbd != nullptr) {
            perf_read("virtio-blk", *vbd, perf_buf);
        }
        auto* nbd = cinux::drivers::nvme::nvme_block_device();
        if (nbd != nullptr) {
            perf_read("NVMe", *nbd, perf_buf);
        }
        if (cinux::drivers::ahci::AHCI::is_present()) {
            auto ahci_blk = cinux::drivers::ahci::AHCIBlockDevice::create(
                cinux::drivers::ahci::AHCI::instance(), 0);
            if (ahci_blk.ok()) {
                perf_read("AHCI", ahci_blk.value(), perf_buf);
            }
        }
    }

    launch_userspace();

    // Unreachable in the non-GUI build: launch_userspace jumps to user mode
    // (busybox init).  Kept as a safety net for any future launch_userspace
    // variant that returns.
    Scheduler::exit_current();
}

}  // namespace cinux::proc
