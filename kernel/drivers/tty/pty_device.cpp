/**
 * @file kernel/drivers/tty/pty_device.cpp
 * @brief PTY device layer: pair registry + master/slave/ptmx InodeOps (batch 3)
 *
 * Kernel-only wiring of the pure Pty onto the VFS device-inode model.  A fixed
 * table of slots owns each pair's Pty plus its master/slave Inodes; the InodeOps
 * recover the Pty from inode->fs_private.  /dev/ptmx is a cloning device: its
 * open() allocates a pair and hands back the master inode.  See pty_device.hpp.
 */

#include "kernel/drivers/tty/pty_device.hpp"

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (slave ioctl)
#include "kernel/drivers/tty/pty.hpp"
#include "kernel/drivers/tty/tty.hpp"  // Termios/Winsize/kTcgets/kTiocsctty...
#include "kernel/fs/devfs.hpp"         // kSIfChr / devfs_makedev
#include "kernel/fs/inode.hpp"
#include "kernel/lib/string.hpp"      // memset for stat
#include "kernel/proc/process.hpp"    // Task::session_leader / controlling_tty
#include "kernel/proc/scheduler.hpp"  // Scheduler::current()

// `struct stat` lives in cinux::fs; pull it into view so the unqualified
// `struct stat` in these InodeOps overrides (parsed in cinux::drivers) matches
// the base signature exactly. devfs.cpp gets this for free by living in fs::.
using cinux::fs::stat;

namespace cinux::drivers {

namespace {

// PTY inode numbers live in a high range so they never collide with DevFs's own
// 1..N device-node inos; the pty index is recoverable as (ino - base).
constexpr uint64_t kPtyInoBase = 0x1000;

// Linux char-device majors (reported via st_rdev; identification only).
constexpr uint32_t kPtmxMajor      = 5;    // /dev/ptmx
constexpr uint32_t kPtySlaveMajor  = 136;  // /dev/pts/N
constexpr uint32_t kPtyMasterMajor = 128;  // legacy master (unused on the fly)

struct PtySlot {
    bool             in_use{false};
    Pty              pty;
    cinux::fs::Inode master_inode{};
    cinux::fs::Inode slave_inode{};
};

PtySlot g_slots[kMaxPtys];

Pty* pty_of(const cinux::fs::Inode* inode) {
    return static_cast<Pty*>(inode->fs_private);
}

void fill_pty_stat(const cinux::fs::Inode* inode, struct stat* st, uint64_t rdev) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->ino;
    st->st_nlink   = 1;
    st->st_mode    = inode->mode;  // set at alloc (kSIfChr | perms)
    st->st_rdev    = rdev;
    st->st_blksize = 4096;
}

// ============================================================
// Master side (the terminal-emulator fd returned by /dev/ptmx open)
// ============================================================

class PtyMasterOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode* inode, uint64_t, void* buf,
                                      uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        return pty_of(inode)->master_read(buf, count);
    }
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t, const void* buf,
                                       uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        return pty_of(inode)->master_write(buf, count);
    }
    cinux::lib::ErrorOr<int64_t> ioctl(const cinux::fs::Inode* inode, uint32_t request,
                                       uint64_t arg) override {
        if (request == kTiocgptn) {
            int n = static_cast<int>(inode->ino - kPtyInoBase);
            // copy failure -> InvalidArgument (EINVAL). Linux returns EFAULT here;
            // the Cinux-Base Error enum has no Fault variant, so this approximates.
            if (!cinux::user::copy_to_user(reinterpret_cast<void*>(arg), &n, sizeof(n))) {
                return cinux::lib::Error::InvalidArgument;
            }
            return 0;
        }
        return cinux::lib::Error::NotImplemented;  // -> -ENOTTY
    }
    cinux::lib::ErrorOr<void> stat(const cinux::fs::Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_pty_stat(inode, st,
                      cinux::fs::devfs_makedev(kPtyMasterMajor, inode->ino - kPtyInoBase));
        return {};
    }
};

// ============================================================
// Slave side (the application's controlling terminal, /dev/pts/N)
// ============================================================

class PtySlaveOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode* inode, uint64_t, void* buf,
                                      uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        Pty* p = pty_of(inode);
        // Non-blocking: drain whatever cooked line is ready (0 if none).  EOF
        // (^D on an empty line) surfaces as a single 0 return via take_eof.
        // Blocking-on-empty (like console_tty) is a scheduling refinement left
        // for the consumer wiring; the batch-3 test drives master-before-slave.
        auto r = p->slave_read(buf, count);
        if (!r.ok()) {
            return r.error();
        }
        if (*r == 0 && p->slave_tty().take_eof()) {
            return static_cast<int64_t>(0);  // EOF
        }
        return r;
    }
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t, const void* buf,
                                       uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        return pty_of(inode)->slave_write(buf, count);
    }
    cinux::lib::ErrorOr<int64_t> ioctl(const cinux::fs::Inode* inode, uint32_t request,
                                       uint64_t arg) override {
        Pty*  p    = pty_of(inode);
        void* uptr = reinterpret_cast<void*>(arg);
        switch (request) {
        case cinux::drivers::kTcgets: {
            const Termios& tm = p->slave_tty().termios();
            if (!cinux::user::copy_to_user(uptr, &tm, sizeof(Termios))) {
                return cinux::lib::Error::InvalidArgument;  // ~EFAULT (no Fault enum)
            }
            return 0;
        }
        case cinux::drivers::kTcsets: {
            Termios tm;
            if (!cinux::user::copy_from_user(&tm, uptr, sizeof(Termios))) {
                return cinux::lib::Error::InvalidArgument;
            }
            p->slave_tty().set_termios(tm);
            return 0;
        }
        case cinux::drivers::kTiocgwinsz: {
            constexpr Winsize kPtyWinsize{25, 80, 0, 0};
            if (!cinux::user::copy_to_user(uptr, &kPtyWinsize, sizeof(Winsize))) {
                return cinux::lib::Error::InvalidArgument;
            }
            return 0;
        }
        case cinux::drivers::kTiocsctty: {
            // Acquire this PTY slave as the caller's controlling terminal.
            // Non-forcing (arg == 0): caller must be a session leader (setsid
            // first) and not already control a tty.  arg == 1 forces a steal.
            // Refusal maps to EACCES (Linux returns EPERM; the Cinux-Base Error
            // enum has no EPERM variant -- PermissionDenied -> EACCES is the
            // closest approximation; the refuse behaviour is what matters).
            cinux::proc::Task* task = cinux::proc::Scheduler::current();
            if (task == nullptr) {
                return cinux::lib::Error::InvalidArgument;
            }
            int  index = static_cast<int>(inode->ino - kPtyInoBase);
            bool steal = (arg != 0);
            if (!steal) {
                if (task->session_leader != task) {
                    return cinux::lib::Error::PermissionDenied;  // not a session leader
                }
                if (task->controlling_tty >= 0) {
                    return cinux::lib::Error::PermissionDenied;  // already controls a tty
                }
            }
            task->controlling_tty = index;
            return 0;
        }
        default:
            return cinux::lib::Error::NotImplemented;  // -> -ENOTTY
        }
    }
    cinux::lib::ErrorOr<void> stat(const cinux::fs::Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_pty_stat(inode, st,
                      cinux::fs::devfs_makedev(kPtySlaveMajor, inode->ino - kPtyInoBase));
        return {};
    }
};

// ============================================================
// /dev/ptmx -- the cloning device
// ============================================================

class PtmxOps : public cinux::fs::InodeOps {
public:
    cinux::lib::ErrorOr<cinux::fs::Inode*> open(cinux::fs::Inode* /*self*/,
                                                uint64_t /*flags*/) override {
        auto idx = pty_alloc();
        if (!idx.ok()) {
            return idx.error();
        }
        return pty_master_inode(*idx);
    }
    cinux::lib::ErrorOr<int64_t> read(const cinux::fs::Inode*, uint64_t, void*, uint64_t) override {
        return cinux::lib::Error::NotImplemented;  // ptmx is open-only; its fd is the master
    }
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode*, uint64_t, const void*,
                                       uint64_t) override {
        return cinux::lib::Error::NotImplemented;
    }
    cinux::lib::ErrorOr<void> stat(const cinux::fs::Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        fill_pty_stat(inode, st, cinux::fs::devfs_makedev(kPtmxMajor, 2));
        return {};
    }
};

PtyMasterOps g_master_ops;
PtySlaveOps  g_slave_ops;
PtmxOps      g_ptmx_ops;

void wire_slot(PtySlot& slot, size_t index) {
    slot.pty.reset();  // clean state for a reused slot; re-anchors echo to &pty
    slot.master_inode.ino        = kPtyInoBase + index;
    slot.master_inode.type       = cinux::fs::InodeType::Regular;
    slot.master_inode.ops        = &g_master_ops;
    slot.master_inode.fs_private = &slot.pty;
    slot.master_inode.mode       = cinux::fs::kSIfChr | 0620;
    slot.master_inode.nlink      = 1;
    slot.slave_inode.ino         = kPtyInoBase + index;
    slot.slave_inode.type        = cinux::fs::InodeType::Regular;
    slot.slave_inode.ops         = &g_slave_ops;
    slot.slave_inode.fs_private  = &slot.pty;
    slot.slave_inode.mode        = cinux::fs::kSIfChr | 0620;
    slot.slave_inode.nlink       = 1;
}

}  // anonymous namespace

cinux::lib::ErrorOr<int> pty_alloc() {
    for (size_t i = 0; i < kMaxPtys; ++i) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use = true;
            wire_slot(g_slots[i], i);
            return static_cast<int>(i);
        }
    }
    return cinux::lib::Error::OutOfMemory;  // table full -> EMFILE
}

void pty_release(int index) {
    if (index >= 0 && static_cast<size_t>(index) < kMaxPtys) {
        g_slots[index].in_use = false;
    }
}

cinux::fs::Inode* pty_master_inode(int index) {
    if (index < 0 || static_cast<size_t>(index) >= kMaxPtys || !g_slots[index].in_use) {
        return nullptr;
    }
    return &g_slots[index].master_inode;
}

cinux::lib::ErrorOr<cinux::fs::Inode*> pty_slave_inode(int index) {
    if (index < 0 || static_cast<size_t>(index) >= kMaxPtys || !g_slots[index].in_use) {
        return cinux::lib::Error::NotFound;
    }
    return &g_slots[index].slave_inode;
}

cinux::fs::InodeOps& ptmx_ops() {
    return g_ptmx_ops;
}

}  // namespace cinux::drivers
