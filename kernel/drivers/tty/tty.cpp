/**
 * @file kernel/drivers/tty/tty.cpp
 * @brief Terminal line discipline implementation (F10-M3 Phase 1)
 *
 * Pure logic -- no kprintf, no Console, no proc::Signal.  Echo is an injected
 * callback and signal requests surface as TtySignal, so the unit links clean in
 * the host test harness.  See tty.hpp for the design.
 */

#include "kernel/drivers/tty/tty.hpp"

namespace cinux::drivers {

// ============================================================
// Default termios (Linux-style cooked mode)
// ============================================================

void make_default_termios(Termios& out) {
    out.c_iflag = 0;
    out.c_oflag = 0;
    out.c_cflag = 0;
    out.c_lflag = kIsig | kIcanon | kEcho | kEchoe | kEchok;
    out.c_line  = 0;
    for (unsigned char& i : out.c_cc) {
        i = 0;
    }
    out.c_cc[kVintr]   = kCharIntr;
    out.c_cc[kVquit]   = kCharQuit;
    out.c_cc[kVerase]  = kCharBackspace;
    out.c_cc[kVkill]   = 0x15;  // ^U
    out.c_cc[kVeof]    = kCharEof;
    out.c_cc[kVsusp]   = kCharSusp;
    out.c_cc[kVwerase] = 0x17;  // ^W
}

// ============================================================
// TTY
// ============================================================

TTY::TTY()
    : termios_{},
      line_buf_{},
      line_len_(0),
      cooked_{},
      cooked_head_(0),
      cooked_tail_(0),
      cooked_full_(false),
      pending_signal_(TtySignal::kNone),
      eof_pending_(false),
      echo_fn_(nullptr),
      echo_ctx_(nullptr) {
    make_default_termios(termios_);
}

void TTY::set_echo_sink(void (*emit)(char c, void* ctx), void* ctx) {
    echo_fn_  = emit;
    echo_ctx_ = ctx;
}

void TTY::echo(char c) const {
    if (echo_fn_ != nullptr) {
        echo_fn_(c, echo_ctx_);
    }
}

bool TTY::cooked_push(char c) {
    if (cooked_full_) {
        return false;  // drop (queue full)
    }
    cooked_[cooked_tail_] = c;
    cooked_tail_          = (cooked_tail_ + 1) % kCookedBufSize;
    if (cooked_tail_ == cooked_head_) {
        cooked_full_ = true;
    }
    return true;
}

bool TTY::cooked_pop(char& c) {
    if (cooked_head_ == cooked_tail_ && !cooked_full_) {
        return false;  // empty
    }
    c            = cooked_[cooked_head_];
    cooked_head_ = (cooked_head_ + 1) % kCookedBufSize;
    cooked_full_ = false;
    return true;
}

void TTY::commit_line() {
    for (size_t i = 0; i < line_len_; i++) {
        if (!cooked_push(line_buf_[i])) {
            break;  // cooked queue full; drop the rest of the line
        }
    }
    line_len_ = 0;
}

InputResult TTY::input_char(char c) {
    // ISIG: signal chars (^C/^\/^Z) generate a signal and never reach the line.
    if (termios_.c_lflag & kIsig) {
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVintr]) {
            pending_signal_ = TtySignal::kSigint;
            return InputResult::kSignal;
        }
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVquit]) {
            pending_signal_ = TtySignal::kSigquit;
            return InputResult::kSignal;
        }
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVsusp]) {
            pending_signal_ = TtySignal::kSigtstp;
            return InputResult::kSignal;
        }
    }

    if (termios_.c_lflag & kIcanon) {
        // Canonical: accumulate into line_buf_ until a line delimiter.
        if (c == kCharNewline) {
            commit_line();
            cooked_push(c);  // newline is part of the delivered line
            echo(c);
            return InputResult::kLineReady;
        }
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVeof]) {
            // VEOF on an empty line -> EOF; otherwise commit what's buffered
            // (without a trailing newline).
            if (line_len_ == 0) {
                eof_pending_ = true;
                return InputResult::kEof;
            }
            commit_line();
            return InputResult::kLineReady;
        }
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVerase]) {
            if (line_len_ > 0) {
                line_len_--;
                if (termios_.c_lflag & kEchoe) {
                    echo('\b');
                    echo(' ');
                    echo('\b');
                }
            }
            return InputResult::kConsumed;
        }
        if (static_cast<uint8_t>(c) == termios_.c_cc[kVkill]) {
            // ^U: erase the whole line being edited.
            while (line_len_ > 0) {
                line_len_--;
                if (termios_.c_lflag & kEchoe) {
                    echo('\b');
                    echo(' ');
                    echo('\b');
                }
            }
            return InputResult::kConsumed;
        }
        // Printable: append + echo.
        if (line_len_ < kLineBufSize) {
            line_buf_[line_len_++] = c;
            if (termios_.c_lflag & kEcho) {
                echo(c);
            }
            return InputResult::kConsumed;
        }
        return InputResult::kOverflow;
    }

    // Raw (~ICANON): each byte goes straight to the cooked queue.
    cooked_push(c);
    if (termios_.c_lflag & kEcho) {
        echo(c);
    }
    return InputResult::kLineReady;
}

size_t TTY::read_cooked(char* buf, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen) {
        char c;
        if (!cooked_pop(c)) {
            break;
        }
        buf[n++] = c;
    }
    return n;
}

TtySignal TTY::pending_signal() const {
    return pending_signal_;
}

TtySignal TTY::take_signal() {
    TtySignal s     = pending_signal_;
    pending_signal_ = TtySignal::kNone;
    return s;
}

bool TTY::take_eof() {
    bool was     = eof_pending_;
    eof_pending_ = false;
    return was;
}

const Termios& TTY::termios() const {
    return termios_;
}

void TTY::set_termios(const Termios& t) {
    termios_        = t;
    // A termios change can invalidate the line being edited.
    line_len_       = 0;
    pending_signal_ = TtySignal::kNone;
    eof_pending_    = false;
}

}  // namespace cinux::drivers
