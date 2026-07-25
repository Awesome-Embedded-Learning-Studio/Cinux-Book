/**
 * @file kernel/syscall/poll_core.cpp
 * @brief Shared poll(2)/select(2) engine implementation (F8-M5)
 *
 * See poll_core.hpp.  do_poll_core() is the single place that knows how to (a)
 * ask each fd whether it is ready (InodeOps::poll_events) and (b) block until one
 * is, parking the poller on every pollable fd's wait queue (the prepare_to_wait
 * + schedule_blocked triplet) so a producer on ANY of them wakes it.
 *
 * The infinite-timeout path is true event-driven sleep (no spin); the finite
 * path yields between scans.  #DF-safe -- never sti/hlt inside the syscall path.
 */

#include "kernel/syscall/poll_core.hpp"

#include <stdint.h>

#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"          // monotonic fallback when HPET is absent
#include "kernel/drivers/tty/console_tty.hpp"  // legacy console stdin (fds 0/1/2)
#include "kernel/errno.hpp"                    // kEintr
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"  // current_fd_table()
#include "kernel/proc/process.hpp"  // Task + TaskState + signal_deliverable_pending
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"         // InterruptGuard
#include "kernel/proc/timer_queue.hpp"  // timer_queue_arm/disarm (finite-timeout wake)

namespace cinux::syscall {

namespace {

constexpr uint64_t kNsPerMs = 1'000'000ULL;

/// Boot-relative monotonic nanoseconds, HPET-backed with a PIT fallback (the
/// same source sys_nanosleep / sys_clock_gettime use; duplicated so this TU
/// pulls in no other handler's .cpp).
uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}

/// Bits always reported in revents regardless of the requested @c events.
constexpr uint16_t always_bits = cinux::fs::kPollErr | cinux::fs::kPollHup | cinux::fs::kPollNval;

/// Resolve one fd to a poll target: an inode-backed fd, the legacy console TTY
/// (fds 0/1/2 with no fd-table entry -- the boot/test stdin), or nothing.
enum class FdKind {
    kNone,
    kInode,
    kConsole
};
struct FdRef {
    FdKind            kind;
    cinux::fs::Inode* inode;
};

FdRef resolve_fd(int fd) {
    if (fd < 0) {
        return {FdKind::kNone, nullptr};
    }
    cinux::fs::File* file = cinux::fs::current_fd_table().get(fd);
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        return {FdKind::kInode, file->inode};
    }
    if (fd <= 2) {
        return {FdKind::kConsole, nullptr};  // legacy console stdin/stdout/stderr
    }
    return {FdKind::kNone, nullptr};
}

/// Readiness-only mask for one fd (no wait registration).  Returns the revents
/// the caller would see: requested-and-ready bits, plus POLLERR/HUP/NVAL.
uint16_t poll_one(int fd, uint16_t events) {
    if (fd < 0) {
        return 0;  // negative fd: ignored, revents left at 0 (Linux)
    }
    FdRef    r      = resolve_fd(fd);
    uint32_t wanted = events | always_bits;
    switch (r.kind) {
    case FdKind::kInode:
        return static_cast<uint16_t>(r.inode->ops->poll_events(r.inode, nullptr, nullptr) & wanted);
    case FdKind::kConsole:
        return static_cast<uint16_t>(cinux::drivers::console_tty().poll_events(nullptr, nullptr) &
                                     wanted);
    default:
        return cinux::fs::kPollNval;  // absent fd > 2 -> invalid
    }
}

/// Register @p waiter on every pollable fd's wait queue and re-check readiness
/// under each fd's lock (the prepare_to_wait contract).  Sets *@p registered if
/// any fd queued the waiter; returns true if any fd was observed ready.
bool register_all(kpollfd* pfds, uint64_t nfds, cinux::proc::Task* waiter, bool* registered) {
    *registered = false;
    bool ready  = false;
    for (uint64_t i = 0; i < nfds; ++i) {
        FdRef    r      = resolve_fd(pfds[i].fd);
        uint32_t wanted = static_cast<uint16_t>(pfds[i].events) | always_bits;
        bool     reg    = false;
        uint32_t raw    = 0;
        switch (r.kind) {
        case FdKind::kInode:
            raw = r.inode->ops->poll_events(r.inode, waiter, &reg);
            break;
        case FdKind::kConsole:
            raw = cinux::drivers::console_tty().poll_events(waiter, &reg);
            break;
        default:
            continue;
        }
        if (reg) {
            *registered = true;
        }
        if ((raw & wanted) != 0) {
            ready = true;
        }
    }
    return ready;
}

/// Detach @p waiter from every fd's wait queue (no-op for non-pollable types).
void detach_all(kpollfd* pfds, uint64_t nfds, cinux::proc::Task* waiter) {
    for (uint64_t i = 0; i < nfds; ++i) {
        FdRef r = resolve_fd(pfds[i].fd);
        switch (r.kind) {
        case FdKind::kInode:
            r.inode->ops->poll_detach_waiter(r.inode, waiter);
            break;
        case FdKind::kConsole:
            cinux::drivers::console_tty().poll_detach(waiter);
            break;
        default:
            break;
        }
    }
}

}  // namespace

int64_t do_poll_core(kpollfd* pfds, uint64_t nfds, int64_t timeout_ms) {
    const bool     infinite = (timeout_ms < 0);
    const uint64_t deadline = (infinite || timeout_ms == 0)
                                  ? 0
                                  : monotonic_ns() + static_cast<uint64_t>(timeout_ms) * kNsPerMs;

    for (;;) {
        // Pass 1: readiness check, no registration (IRQs on).
        int64_t ready = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            pfds[i].revents = poll_one(pfds[i].fd, static_cast<uint16_t>(pfds[i].events));
            if (pfds[i].revents != 0) {
                ++ready;
            }
        }
        if (ready > 0) {
            return ready;
        }
        if (!infinite && monotonic_ns() >= deadline) {
            return 0;  // timed out (or timeout==0 single pass)
        }

        // Unified park (finite AND infinite): register on every pollable fd's
        // wait queue, and -- for a finite timeout -- also arm the timer-wake.
        // Then schedule_blocked(); the first of {fd event, timer} to fire wakes
        // us (Scheduler::unblock is idempotent, so the loser is a no-op).  This
        // is true event-driven sleep with no yield/spin, even for finite
        // timeouts (the F5-M4 timer follow-up, now in place).
        cinux::proc::Task* self = cinux::proc::Scheduler::current();
        if (self == nullptr) {
            return 0;  // no scheduler context (early boot) -- can't block
        }
        bool registered   = false;
        bool became_ready = false;
        bool armed        = false;
        bool will_sleep   = false;
        {
            // IRQs off across prepare_to_wait + registration + arm: no tick can
            // preempt a Blocked task before it is on a queue / armed (lost
            // wakeup), and the waiter lands on every queue before IRQs return.
            cinux::proc::InterruptGuard ig;
            cinux::proc::Scheduler::prepare_to_wait(self);  // state -> Blocked
            became_ready = register_all(pfds, nfds, self, &registered);
            if (!infinite) {
                armed = cinux::proc::timer_queue_arm(self, deadline);
            }
            // Sleep unless a fd is already ready, or (infinite with nothing that
            // could ever wake us -- avoid a permanent hang).
            will_sleep = !became_ready && (registered || armed);
            if (!will_sleep) {
                self->state = cinux::proc::TaskState::Running;  // cancel the prepare
            }
        }  // IRQs restored
        if (!will_sleep) {
            if (became_ready) {
                detach_all(pfds, nfds, self);
                continue;  // pass 1 will report the ready fd
            }
            return 0;  // infinite, nothing registered & no timer -> no wake source
        }
        cinux::proc::Scheduler::schedule_blocked();  // switch out; woken by fd or timer
        detach_all(pfds, nfds, self);
        if (armed) {
            cinux::proc::timer_queue_disarm(self);  // free the timer slot
        }
        // EINTR: a signal landed while parked.  Return -EINTR (POSIX poll is
        // interruptible).  detach_all already unlinked us from every fd's wait
        // queue, so no stale link remains.  We do NOT consume the signal here --
        // the syscall return path (signal_check_and_deliver) runs it next.
        if (signal_deliverable_pending(self)) {
            return -cinux::kEintr;
        }
        // loop: re-scan (a fd is ready, or the timeout expired)
    }
}

}  // namespace cinux::syscall
