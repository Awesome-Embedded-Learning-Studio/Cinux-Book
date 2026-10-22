#include "kernel/driver/tty/pty.hpp"

#include "kernel/driver/tty/tty.hpp"

namespace cinux::driver {

Pty::Pty(void (*wait)(void* context)) {
    wiring_.context  = this;
    wiring_.take_key = &Pty::take_raw;
    wiring_.has_key  = &Pty::peek_raw;
    wiring_.echo     = &Pty::push_echo;
    wiring_.wait     = wait;
    slave_.wire(wiring_);
}

Tty& Pty::slave() {
    return slave_;
}

bool Pty::take_raw(void* context, char* out) {
    auto* const kPair = static_cast<Pty*>(context);
    return kPair->raw_.pop(*reinterpret_cast<unsigned char*>(out));
}

bool Pty::peek_raw(void* context) {
    const auto* const kPair = static_cast<Pty*>(context);
    return kPair->raw_.count() != 0;
}

void Pty::push_echo(void* context, char key) {
    auto* const kPair = static_cast<Pty*>(context);
    static_cast<void>(kPair->out_.push(static_cast<unsigned char>(key)));
}

unsigned long Pty::master_write(const void* source, unsigned long count) {
    auto const* const kIn    = static_cast<const unsigned char*>(source);
    unsigned long     landed = 0;
    while (landed < count && raw_.push(kIn[landed])) {
        ++landed;
    }
    return landed;
}

unsigned long Pty::master_read(void* sink, unsigned long count) {
    auto* const   kOut  = static_cast<unsigned char*>(sink);
    unsigned long taken = 0;
    while (taken < count && out_.pop(kOut[taken])) {
        ++taken;
    }
    return taken;
}

unsigned long Pty::slave_write(const void* source, unsigned long count) {
    auto const* const kIn    = static_cast<const unsigned char*>(source);
    unsigned long     landed = 0;
    while (landed < count && out_.push(kIn[landed])) {
        ++landed;
    }
    return landed;
}

bool Pty::master_has_input() const {
    return out_.count() != 0 || slave_closed();
}

bool Pty::raw_has_keys() const {
    return raw_.count() != 0;
}

void Pty::retain_master() {
    master_opens_.fetch_add(1);
}

void Pty::release_master() {
    if (master_opens_.fetch_sub(1) == 1) {
        slave_.hang_up();
    }
}

void Pty::retain_slave() {
    unsigned long opens = slave_opens_.load();
    for (;;) {
        const unsigned long kNext = opens == kSlaveNeverOpened ? 1 : opens + 1;
        if (slave_opens_.compare_exchange_weak(opens, kNext)) {
            return;
        }
    }
}

void Pty::release_slave() {
    slave_opens_.fetch_sub(1);
}

void Pty::reset() {
    master_opens_.store(0);
    slave_opens_.store(kSlaveNeverOpened);
    unsigned char byte = 0;
    while (raw_.pop(byte)) {
    }
    while (out_.pop(byte)) {
    }
    slave_.wire(wiring_);
    slave_.set_window({});
}

}  // namespace cinux::driver
