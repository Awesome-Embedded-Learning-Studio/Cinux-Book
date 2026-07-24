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
#include "kernel/fs/devfs/devfs.hpp"   // kSIfChr / devfs_makedev
#include "kernel/fs/inode.hpp"
#include "kernel/lib/echo_trace.hpp"
#include "kernel/lib/string.hpp"      // memset for stat
#include "kernel/proc/process.hpp"    // Task::session_leader / controlling_tty
#include "kernel/proc/scheduler.hpp"  // Scheduler::current()
#include "kernel/proc/signal.hpp"     // killpg + Signal (foreground pgrp delivery)
#include "kernel/proc/sync.hpp"       // Spinlock

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
    bool                  in_use{false};
    Pty                   pty;
    cinux::fs::Inode      master_inode{};
    cinux::fs::Inode      slave_inode{};
    cinux::proc::Spinlock lock{};
    cinux::proc::Task*    slave_read_waiters{nullptr};
    // Foreground process group of this terminal (POSIX tcsetpgrp / TIOCSPGRP).
    // ^C/^\/^Z typed on the master are delivered here via killpg().  Defaults to
    // the session leader's pgrp when the slave is acquired as a controlling tty
    // (TIOCSCTTY), so signal chars reach the foreground job even before the
    // shell issues TIOCSPGRP.  0 = unset (no signal delivery yet).
    int                   foreground_pgid{0};
};

PtySlot g_slots[kMaxPtys];

Pty* pty_of(const cinux::fs::Inode* inode) {
    return static_cast<Pty*>(inode->fs_private);
}

PtySlot* slot_of(const cinux::fs::Inode* inode) {
    if (inode == nullptr || inode->ino < kPtyInoBase) {
        return nullptr;
    }
    size_t index = static_cast<size_t>(inode->ino - kPtyInoBase);
    if (index >= kMaxPtys) {
        return nullptr;
    }
    return &g_slots[index];
}

void wait_enqueue(cinux::proc::Task*& head, cinux::proc::Task* task) {
    task->wait_next = nullptr;
    if (head == nullptr) {
        head = task;
        return;
    }
    cinux::proc::Task* tail = head;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

cinux::proc::Task* wait_dequeue(cinux::proc::Task*& head) {
    cinux::proc::Task* task = head;
    if (task != nullptr) {
        head            = task->wait_next;
        task->wait_next = nullptr;
    }
    return task;
}

void wake_all(cinux::proc::Task*& head) {
    while (cinux::proc::Task* task = wait_dequeue(head)) {
        cinux::proc::Scheduler::unblock(task);
    }
}

// Map a slave line-discipline signal char to its POSIX signal number.  Returns
// false for kNone (no signal requested this write).  Mirrors console_tty.cpp's
// switch, but lives here because the PTY master, not the console, owns delivery
// for PTY-backed shells (the GUI terminals).
bool tty_signal_to_signo(TtySignal ts, cinux::proc::Signal& out) {
    switch (ts) {
    case TtySignal::kSigint:  out = cinux::proc::Signal::kSigint;  return true;
    case TtySignal::kSigquit: out = cinux::proc::Signal::kSigquit; return true;
    case TtySignal::kSigtstp: out = cinux::proc::Signal::kSigtstp; return true;
    default: return false;  // kNone
    }
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
        PtySlot* slot = slot_of(inode);
        if (slot == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        auto guard = slot->lock.irq_guard();
        auto r     = slot->pty.master_read(buf, count);
        if (r.ok() && *r > 0) {
            cinux::debug::trace_bytes("pty.master_read to_terminal", buf, *r);
        }
        return r;
    }
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t, const void* buf,
                                       uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        PtySlot* slot = slot_of(inode);
        if (slot == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        auto r = cinux::lib::ErrorOr<int64_t>(cinux::lib::Error::InvalidArgument);
        // A signal char (^C/^\/^Z) typed on the master is latched by the slave
        // line discipline as a TtySignal.  The pure Pty only sets it aside -- the
        // kernel wiring must DELIVER it (mirrors console_tty::input's killpg).
        // Snapshot the signal + foreground pgrp under slot->lock, then deliver
        // AFTER releasing: killpg takes the registry lock and may unblock the
        // target, none of which needs slot->lock.
        TtySignal pending_sig = TtySignal::kNone;
        int       fg_pgid     = 0;
        {
            auto guard = slot->lock.irq_guard();
            cinux::debug::trace_bytes("pty.master_write from_terminal", buf,
                                      static_cast<int64_t>(count));
            r = slot->pty.master_write(buf, count);
            if (r.ok() && *r > 0) {
                wake_all(slot->slave_read_waiters);
            }
            pending_sig = slot->pty.take_pending_signal();
            fg_pgid     = slot->foreground_pgid;
        }
        cinux::proc::Signal sig;
        if (pending_sig != TtySignal::kNone && fg_pgid > 0 && tty_signal_to_signo(pending_sig, sig)) {
            cinux::proc::killpg(fg_pgid, sig);
        }
        return r;
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
        PtySlot* slot = slot_of(inode);
        if (slot == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }

        for (;;) {
            bool need_block = false;
            {
                auto guard = slot->lock.irq_guard();
                if (!slot->in_use) {
                    return static_cast<int64_t>(0);  // master side released
                }

                auto r = slot->pty.slave_read(buf, count);
                if (!r.ok()) {
                    return r.error();
                }
                if (*r > 0) {
                    cinux::debug::trace_bytes("pty.slave_read to_shell", buf, *r);
                    return r;
                }
                if (slot->pty.slave_tty().take_eof()) {
                    return static_cast<int64_t>(0);  // ^D on an empty line -> EOF
                }

                cinux::proc::Task* self = cinux::proc::Scheduler::current();
                if (self == nullptr) {
                    return r;  // early/test context with no scheduler to park on
                }
                wait_enqueue(slot->slave_read_waiters, self);
                cinux::proc::Scheduler::prepare_to_wait(self);
                need_block = true;
            }  // IRQs restored and PTY lock released before switching out.

            if (need_block) {
                cinux::proc::Scheduler::schedule_blocked();
            }
        }
    }
    cinux::lib::ErrorOr<int64_t> write(cinux::fs::Inode* inode, uint64_t, const void* buf,
                                       uint64_t count) override {
        if (buf == nullptr && count > 0) {
            return cinux::lib::Error::InvalidArgument;
        }
        PtySlot* slot = slot_of(inode);
        if (slot == nullptr) {
            return cinux::lib::Error::InvalidArgument;
        }
        auto guard = slot->lock.irq_guard();
        auto r     = slot->pty.slave_write(buf, count);
        if (r.ok() && *r > 0) {
            cinux::debug::trace_bytes("pty.slave_write from_shell", buf, *r);
        }
        return r;
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
            // The acquiring session leader's pgrp is the initial foreground group
            // of this terminal (POSIX: until tcsetpgrp changes it).  Lets ^C typed
            // on the master reach the foreground job even before the shell issues
            // TIOCSPGRP (and regardless of whether the shell uses job control).
            if (PtySlot* slot = slot_of(inode)) {
                auto g = slot->lock.irq_guard();
                if (slot->foreground_pgid == 0) {
                    slot->foreground_pgid = task->pgid;
                }
            }
            return 0;
        }
        case cinux::drivers::kTiocspgrp: {
            // tcsetpgrp: name the foreground process group of this terminal.  A
            // shell with job control sets this to the foreground job's pgrp so ^C
            // reaches it (not the shell).
            int pgid;
            if (!cinux::user::copy_from_user(&pgid, uptr, sizeof(int))) {
                return cinux::lib::Error::InvalidArgument;  // ~EFAULT
            }
            if (pgid < 0) {
                return cinux::lib::Error::InvalidArgument;  // EINVAL
            }
            PtySlot* slot = slot_of(inode);
            if (slot == nullptr) {
                return cinux::lib::Error::InvalidArgument;
            }
            auto g = slot->lock.irq_guard();
            slot->foreground_pgid = pgid;
            return 0;
        }
        case cinux::drivers::kTiocgpgrp: {
            // tcgetpgrp: read the foreground process group of this terminal.
            PtySlot* slot = slot_of(inode);
            if (slot == nullptr) {
                return cinux::lib::Error::InvalidArgument;
            }
            int pgid;
            {
                auto g = slot->lock.irq_guard();
                pgid = slot->foreground_pgid;
            }
            if (!cinux::user::copy_to_user(uptr, &pgid, sizeof(int))) {
                return cinux::lib::Error::InvalidArgument;  // ~EFAULT
            }
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
    {
        auto guard = slot.lock.irq_guard();
        slot.pty.reset();  // clean state for a reused slot; re-anchors echo to &pty
        slot.slave_read_waiters = nullptr;
    }
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
        PtySlot& slot  = g_slots[index];
        auto     guard = slot.lock.irq_guard();
        slot.in_use    = false;
        wake_all(slot.slave_read_waiters);
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
