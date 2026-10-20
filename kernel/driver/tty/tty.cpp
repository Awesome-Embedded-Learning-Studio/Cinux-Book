#include "kernel/driver/tty/tty.hpp"

#include <cstdint>

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "kernel/driver/tty/tty_config.hpp"

namespace cinux::driver {

namespace {

/// Bytes the editor treats as erasures: both backspace conventions.
bool is_erase(char key) {
    return key == '\b' || key == 0x7F;
}

/// Bytes the editor treats as line ends: both return conventions.
bool is_line_end(char key) {
    return key == '\r' || key == '\n';
}

/// The lflag word out of a termios record.
std::uint32_t lflag_of(const unsigned char* record) {
    std::uint32_t flags = 0;
    base::CopyBytes(&flags, record + kTermiosLflagAt, sizeof(flags));
    return flags;
}

}  // namespace

Tty::Tty(const TtyWiring& wiring) {
    wire(wiring);
}

void Tty::wire(const TtyWiring& wiring) {
    wiring_         = &wiring;
    used_           = 0;
    pending_signal_ = 0;
    base::SetBytes(termios_, 0, kTermiosBytes);
    const std::uint32_t kBornFlags = kTermiosIsig | kTermiosIcanon | kTermiosEcho;
    base::CopyBytes(termios_ + kTermiosLflagAt, &kBornFlags, sizeof(kBornFlags));
    termios_[kTermiosCcAt + kTermiosVintr]  = kTtyIntrChar;
    termios_[kTermiosCcAt + kTermiosVquit]  = kTtyQuitChar;
    termios_[kTermiosCcAt + kTermiosVerase] = 0x7F;
    termios_[kTermiosCcAt + kTermiosVkill]  = 0x15;
    termios_[kTermiosCcAt + kTermiosVeof]   = 0x04;
    termios_[kTermiosCcAt + kTermiosVmin]   = 1;
    termios_[kTermiosCcAt + kTermiosVsusp]  = kTtySuspChar;
    unsigned char leftover                  = 0;
    while (pool_.pop(leftover)) {
    }
}

void Tty::require_wired() const {
    base::safety::Check(wiring_ != nullptr, "terminal used before wiring");
}

void Tty::echo_char(char key) const {
    if ((lflag_of(termios_) & kTermiosEcho) != 0) {
        wiring_->echo(wiring_->context, key);
    }
}

void Tty::park_line() {
    for (unsigned long index = 0; index < used_; ++index) {
        const auto kOne = static_cast<unsigned char>(line_[index]);
        if (!pool_.push(kOne)) {
            break;
        }
    }
    used_ = 0;
}

int Tty::take_pending_signal() {
    const int kHeld = pending_signal_;
    pending_signal_ = 0;
    return kHeld;
}

bool Tty::feed(char key) {
    if ((lflag_of(termios_) & kTermiosIsig) != 0) {
        const int kSignal = signal_for(key);
        if (kSignal != 0) {
            if (pending_signal_ == 0) {
                pending_signal_ = kSignal;
            }
            if ((lflag_of(termios_) & kTermiosNoflsh) == 0) {
                used_                   = 0;
                unsigned char discarded = 0;
                while (pool_.pop(discarded)) {
                }
            }
            return false;
        }
    }
    if ((lflag_of(termios_) & kTermiosIcanon) == 0) {
        echo_char(key);
        const auto kOne = static_cast<unsigned char>(key);
        pool_.push(kOne);
        return key == '\n' || key == '\r';
    }
    if (is_erase(key)) {
        if (used_ > 0) {
            --used_;
            echo_char('\b');
            echo_char(' ');
            echo_char('\b');
        }
        return false;
    }
    if (is_line_end(key)) {
        if (used_ + 1 < kTtyLineMax) {
            line_[used_] = '\n';
            ++used_;
        }
        park_line();
        echo_char('\n');
        return true;
    }
    if (used_ + 1 < kTtyLineMax) {
        line_[used_] = key;
        ++used_;
        echo_char(key);
    }
    return false;
}

int Tty::signal_for(char key) const {
    const auto kByte = static_cast<unsigned char>(key);
    if (kByte == 0) {
        return 0;
    }
    if (kByte == termios_[kTermiosCcAt + kTermiosVintr]) {
        return 2;
    }
    if (kByte == termios_[kTermiosCcAt + kTermiosVquit]) {
        return 3;
    }
    return kByte == termios_[kTermiosCcAt + kTermiosVsusp] ? 20 : 0;
}

bool Tty::has_input() const {
    require_wired();
    if (pool_.count() != 0 || used_ != 0 || pending_signal_ != 0) {
        return true;
    }
    return wiring_->has_key(wiring_->context);
}

unsigned long Tty::read_line(unsigned char* sink, unsigned long count) {
    require_wired();
    if (pending_signal_ != 0 || count == 0) {
        return 0;
    }
    while (pool_.count() == 0) {
        char key = 0;
        if (!wiring_->take_key(wiring_->context, &key)) {
            wiring_->wait(wiring_->context);
            continue;
        }
        feed(key);
        if (pending_signal_ != 0) {
            return 0;
        }
    }
    unsigned long handed = 0;
    while (handed < count && pool_.pop(sink[handed])) {
        ++handed;
        if ((lflag_of(termios_) & kTermiosIcanon) != 0 && sink[handed - 1] == '\n') {
            break;
        }
    }
    return handed;
}

void Tty::copy_termios(unsigned char* sink) const {
    base::CopyBytes(sink, termios_, kTermiosBytes);
}

void Tty::apply_termios(const unsigned char* source) {
    base::CopyBytes(termios_, source, kTermiosBytes);
}

}  // namespace cinux::driver
