#include "kernel/driver/tty/tty.hpp"

#include "kernel/console/console.hpp"
#include "kernel/driver/keyboard/keyboard.hpp"
#include "kernel/driver/serial/serial.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::driver {

namespace {

/// Keys the editor treats as erasures: both backspace conventions.
bool is_erase(char key) {
    return key == '\b' || key == 0x7F;
}

/// Keys the editor treats as line ends: both return conventions.
bool is_line_end(char key) {
    return key == '\r' || key == '\n';
}

}  // namespace

bool Tty::feed(char key) {
    if (is_erase(key)) {
        if (used_ > 0) {
            --used_;
            console::PutChar('\b');
            console::PutChar(' ');
            console::PutChar('\b');
        }
        return false;
    }
    if (is_line_end(key)) {
        if (used_ + 1 < kTtyLineMax) {
            line_[used_] = '\n';
            ++used_;
        }
        for (unsigned long index = 0; index < used_; ++index) {
            const auto kOne = static_cast<unsigned char>(line_[index]);
            if (!pool_.push(kOne)) {
                break;
            }
        }
        used_ = 0;
        console::PutChar('\n');
        return true;
    }
    if (used_ + 1 < kTtyLineMax) {
        line_[used_] = key;
        ++used_;
        console::PutChar(key);
    }
    return false;
}

namespace {

/// One pass over both input seats: the serial line first, because the
/// hosted world types there, then the PS/2 keyboard the QEMU monitor
/// feeds. True when either seat produced a key.
bool take_any_key(char* out) {
    if (SerialTryGetChar(out)) {
        return true;
    }
    const char kMaybe = Keyboard::self().take(false);
    if (kMaybe != 0) {
        *out = kMaybe;
        return true;
    }
    return false;
}

}  // namespace

bool Tty::has_input() {
    if (pool_.count() != 0 || used_ != 0) {
        return true;
    }
    return SerialHasKey() || Keyboard::self().poll();
}

unsigned long Tty::read_line(unsigned char* sink, unsigned long count) {
    while (pool_.count() == 0) {
        char key = 0;
        if (!take_any_key(&key)) {
            proc::Scheduler::self().yield();
            continue;
        }
        feed(key);
    }
    unsigned long handed = 0;
    while (handed < count && pool_.pop(sink[handed])) {
        if (sink[handed] == '\n') {
            ++handed;
            break;
        }
        ++handed;
    }
    return handed;
}

}  // namespace cinux::driver
