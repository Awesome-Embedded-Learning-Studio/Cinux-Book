#include "kernel/driver/keyboard.hpp"

#include <stdint.h>

#include "kernel/driver/base/io.hpp"
#include "kernel/driver/scancode.hpp"
#include "kernel/interrupt/irq.hpp"

namespace {

constexpr uint16_t kDataPort    = 0x60;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusOutputFull = 0x01;
constexpr uint8_t kStatusInputFull  = 0x02;

constexpr uint8_t kCommandDisableFirst  = 0xAD;
constexpr uint8_t kCommandDisableSecond = 0xA7;
constexpr uint8_t kCommandEnableFirst   = 0xAE;
constexpr uint8_t kCommandSelfTest      = 0xAA;
constexpr uint8_t kCommandReadConfig    = 0x20;
constexpr uint8_t kCommandWriteConfig   = 0x60;
constexpr uint8_t kCommandExpectHealthy = 0x55;

constexpr uint8_t kConfigIrqFirst    = 0x01;
constexpr uint8_t kConfigIrqSecond   = 0x02;
constexpr uint8_t kConfigTranslation = 0x40;

constexpr unsigned int kKeyboardIrqLineValue = 1;

void send_command(uint8_t command) {
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kCommandPort, .value = command});
}

uint8_t read_data() {
    cinux::driver::WaitBitsSet(kCommandPort, kStatusOutputFull);
    return cinux::driver::InB(kDataPort);
}

void write_data(uint8_t value) {
    cinux::driver::WaitBitsClear(kCommandPort, kStatusInputFull);
    cinux::driver::OutB({.port = kDataPort, .value = value});
}

void flush_output() {
    while ((cinux::driver::InB(kCommandPort) & kStatusOutputFull) != 0) {
        (void)cinux::driver::InB(kDataPort);
    }
}

void irq1_thunk() {
    cinux::driver::Keyboard::self().on_byte(cinux::driver::InB(kDataPort));
}

}  // namespace

namespace cinux::driver {

bool Keyboard::init() {
    send_command(kCommandDisableFirst);
    send_command(kCommandDisableSecond);
    flush_output();
    send_command(kCommandReadConfig);
    auto const kConfig = read_data();
    auto const kWanted = static_cast<uint8_t>((kConfig | kConfigIrqFirst | kConfigTranslation) &
                                              static_cast<uint8_t>(~kConfigIrqSecond));
    send_command(kCommandWriteConfig);
    write_data(kWanted);
    send_command(kCommandSelfTest);
    if (read_data() != kCommandExpectHealthy) {
        return false;
    }
    send_command(kCommandEnableFirst);
    return true;
}

void Keyboard::attach() {
    cinux::interrupt::Irq::self().register_handler(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue}, irq1_thunk);
    cinux::interrupt::Irq::self().enable_line(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue});
}

void Keyboard::on_byte(uint8_t raw_byte) {
    char const kGlyph = TranslateScancode(state_, raw_byte);
    if (kGlyph != 0) {
        (void)events_.push(kGlyph);
    }
}

bool Keyboard::poll() const {
    return events_.count() != 0;
}

char Keyboard::take() {
    char code = 0;
    (void)events_.pop(code);
    return code;
}

}  // namespace cinux::driver
