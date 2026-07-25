/**
 * @file kernel/drivers/tty/console_tty.cpp
 * @brief System console TTY: singleton, blocking stdin, signal delivery (F10-M3)
 */

#include "kernel/drivers/tty/console_tty.hpp"

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (console_tty_ioctl)
#include "kernel/fs/inode.hpp"                 // kPollIn
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"  // Task::pgid
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"  // killpg (interrupt/quit/suspend -> foreground pgrp)
#include "kernel/proc/sync.hpp"

namespace cinux::drivers {

namespace {
ConsoleTty g_console_tty;
}  // namespace

ConsoleTty& console_tty() {
    return g_console_tty;
}

void ConsoleTty::echo_via_kprintf(char c, void* /*ctx*/) {
    // kprintf/Console::putc are lock-free, so calling this from the keyboard
    // IRQ context cannot deadlock; a rare re-entrant overlap only risks a
    // transient cursor glitch (deferred-echo is a follow-up).
    cinux::lib::kprintf("%c", c);
}

void ConsoleTty::init() {
    Termios tm;
    make_default_termios(tm);
    tm.c_cc[kVerase] = 0x08;  // keyboard driver emits ^H for the Backspace key
    tty_.set_termios(tm);
    tty_.set_echo_sink(&ConsoleTty::echo_via_kprintf, nullptr);
}

size_t ConsoleTty::read(char* buf, size_t len) {
    auto* self = cinux::proc::Scheduler::current();
    for (;;) {
        {
            // IRQs off: the check + register-as-reader + mark-Blocked must be
            // atomic vs the keyboard feeder, so a line arriving in the window is
            // seen (and a feeder's wakeup finds us already Blocked -> no lost
            // wakeup, per the F3 prepare_to_wait design).
            cinux::proc::InterruptGuard guard;
            size_t                      n = tty_.read_cooked(buf, len);
            if (n > 0) {
                return n;
            }
            if (tty_.take_eof()) {
                return 0;  // Ctrl+D on an empty line -> EOF
            }
            reader_ = self;
            cinux::proc::Scheduler::prepare_to_wait(self);
        }  // IRQs restored BEFORE switching out
        cinux::proc::Scheduler::schedule_blocked();
        // Woken by the feeder (reader_ cleared); loop and re-read.
    }
}

uint32_t ConsoleTty::poll_events(cinux::proc::Task* waiter, bool* registered) {
    // IRQs off: the readiness check + park-as-reader must be atomic vs the
    // keyboard feeder (same window read() closes).  A line arriving in the
    // window is either seen as POLLIN here or finds the waiter already parked
    // and wakes it via input() -- no lost wakeup.
    cinux::proc::InterruptGuard guard;
    bool                        ready = tty_.has_cooked_data();
    if (waiter != nullptr) {
        reader_ = waiter;  // single reader slot (shared with read())
        if (registered != nullptr) {
            *registered = true;
        }
    } else if (registered != nullptr) {
        *registered = false;
    }
    return ready ? cinux::fs::kPollIn : 0u;
}

void ConsoleTty::poll_detach(cinux::proc::Task* waiter) {
    cinux::proc::InterruptGuard guard;
    if (reader_ == waiter) {
        reader_ = nullptr;
    }
}

void ConsoleTty::input(char c) {
    InputResult r = tty_.input_char(c);

    if (r == InputResult::kSignal) {
        // A signal char (interrupt/quit/suspend) maps to a POSIX signal and is
        // delivered to the foreground process group via killpg.  When no
        // foreground group is set, fall back to the blocked reader's group.
        // killpg's registry lock uses irq_guard, so this is safe from the
        // keyboard IRQ context.
        TtySignal           ts  = tty_.take_signal();
        cinux::proc::Signal sig = cinux::proc::Signal::kSigint;
        switch (ts) {
        case TtySignal::kSigquit:
            sig = cinux::proc::Signal::kSigquit;
            break;
        case TtySignal::kSigtstp:
            sig = cinux::proc::Signal::kSigtstp;
            break;
        case TtySignal::kSigint:
            sig = cinux::proc::Signal::kSigint;
            break;
        default:
            return;  // kNone: spurious, nothing to deliver
        }
        int pgid = foreground_pgid_;
        if (pgid == 0 && reader_ != nullptr) {
            pgid = reader_->pgid;
        }
        if (pgid != 0) {
            cinux::proc::killpg(pgid, sig);
        }
        return;
    }

    if (r == InputResult::kLineReady || r == InputResult::kEof) {
        if (reader_ != nullptr) {
            auto* w = reader_;
            reader_ = nullptr;
            cinux::proc::Scheduler::unblock(w);
        }
    }
}

TTY& ConsoleTty::tty() {
    return tty_;
}

int ConsoleTty::foreground_pgid() const {
    return foreground_pgid_;
}

void ConsoleTty::set_foreground_pgid(int pgid) {
    foreground_pgid_ = pgid;
}

cinux::lib::ErrorOr<int64_t> console_tty_ioctl(uint32_t request, uint64_t arg) {
    void*       uptr = reinterpret_cast<void*>(arg);
    ConsoleTty& ct   = console_tty();
    switch (request) {
    case kTcgets: {
        const Termios& tm = ct.tty().termios();
        if (!cinux::user::copy_to_user(uptr, &tm, sizeof(Termios))) {
            return cinux::lib::Error::Fault;  // EFAULT: bad user pointer
        }
        return 0;
    }
    case kTcsets: {
        Termios tm;
        if (!cinux::user::copy_from_user(&tm, uptr, sizeof(Termios))) {
            return cinux::lib::Error::Fault;
        }
        ct.tty().set_termios(tm);
        return 0;
    }
    case kTiocgwinsz: {
        // Console geometry: the live framebuffer Console is not globally
        // reachable from here; 80x25 is the classic Linux text-mode default and
        // all libc needs to pick a buffering mode + wrap width.
        constexpr Winsize kConsoleWinsize{25, 80, 0, 0};
        if (!cinux::user::copy_to_user(uptr, &kConsoleWinsize, sizeof(Winsize))) {
            return cinux::lib::Error::Fault;
        }
        return 0;
    }
    case kTiocgpgrp: {
        int pgid = ct.foreground_pgid();
        if (!cinux::user::copy_to_user(uptr, &pgid, sizeof(int))) {
            return cinux::lib::Error::Fault;
        }
        return 0;
    }
    case kTiocspgrp: {
        int pgid;
        if (!cinux::user::copy_from_user(&pgid, uptr, sizeof(int))) {
            return cinux::lib::Error::Fault;
        }
        if (pgid < 0) {
            return cinux::lib::Error::InvalidArgument;  // EINVAL
        }
        ct.set_foreground_pgid(pgid);
        return 0;
    }
    case kTiocsctty: {
        // Acquire the built-in console as the controlling terminal.  This keeps
        // /dev/tty meaningful in console-only boots where no PTY slave exists.
        auto* task = cinux::proc::Scheduler::current();
        if (task != nullptr) {
            task->controlling_tty = kConsoleControllingTty;
            if (ct.foreground_pgid() == 0 && task->pgid != 0) {
                ct.set_foreground_pgid(task->pgid);
            }
        }
        return 0;
    }
    default:
        return cinux::lib::Error::NotImplemented;  // -> -ENOTTY
    }
}

}  // namespace cinux::drivers
