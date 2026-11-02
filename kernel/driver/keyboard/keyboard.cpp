#include "kernel/driver/keyboard/keyboard.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/driver/keyboard/scancode.hpp"
#include "kernel/driver/ps2/controller.hpp"
#include "kernel/interrupt/irq.hpp"


namespace {

constexpr uint8_t kConfigTranslation = 0x40;

using cinux::driver::ps2::kConfigIrqFirst;
using cinux::driver::ps2::kConfigIrqSecond;
using cinux::driver::ps2::kDataPort;

constexpr unsigned int kKeyboardIrqLineValue = 1;

void irq1_thunk() {
    cinux::driver::Keyboard::self().on_byte(cinux::driver::InB(kDataPort));
}

}  // namespace

namespace cinux::driver {

bool Keyboard::init() {
    const ps2::Ps2Ports kPorts = ps2::HostPs2Ports();
    using ps2::kCommandDisableFirst;
    using ps2::kCommandDisableSecond;
    using ps2::kCommandEnableFirst;
    using ps2::kCommandReadConfig;
    using ps2::kCommandSelfTest;
    using ps2::kCommandWriteConfig;
    using ps2::kSelfTestHealthy;
    if (!ps2::SendCommand(kPorts, kCommandDisableFirst) ||
        !ps2::SendCommand(kPorts, kCommandDisableSecond) || !ps2::Drain(kPorts) ||
        !ps2::SendCommand(kPorts, kCommandReadConfig)) {
        return false;
    }
    uint8_t config = 0;
    if (!ps2::ReadWhenReady(kPorts, config)) {
        return false;
    }
    base::bit::BitMask<uint8_t> wanted{config};
    wanted = (wanted | base::bit::BitMask<uint8_t>{kConfigIrqFirst} |
              base::bit::BitMask<uint8_t>{kConfigTranslation}) &
             ~base::bit::BitMask<uint8_t>{kConfigIrqSecond};
    if (!ps2::SendCommand(kPorts, kCommandWriteConfig) || !ps2::WriteData(kPorts, wanted.raw) ||
        !ps2::SendCommand(kPorts, kCommandSelfTest)) {
        return false;
    }
    uint8_t healthy = 0;
    if (!ps2::ReadWhenReady(kPorts, healthy) || healthy != kSelfTestHealthy) {
        return false;
    }
    return ps2::SendCommand(kPorts, kCommandEnableFirst);
}

void Keyboard::attach() {
    stream_.set_grab_hook(&Keyboard::grab_hook, nullptr);
    cinux::interrupt::Irq::self().register_handler(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue}, irq1_thunk);
    cinux::interrupt::Irq::self().enable_line(
        cinux::interrupt::IrqLine{.value = kKeyboardIrqLineValue});
}

void Keyboard::on_byte(uint8_t raw_byte) {
    char const kGlyph = TranslateScancode(state_, raw_byte);
    if (kGlyph != 0 && glyphs_live_) {
        events_.push(kGlyph);
        readable_.post();
    }
    KeycodeEvent word{};
    if (TranslateScancodeKeycode(codes_, raw_byte, word)) {
        stream_.publish(kEvKey, word.code, word.pressed ? 1 : 0);
        stream_.publish_report();
    }
}

bool Keyboard::poll() const {
    return events_.count() != 0;
}

char Keyboard::take(bool wait) {
    if (wait) {
        while (!poll()) {
            readable_.wait();
        }
    } else if (!poll()) {
        return 0;
    }
    char code = 0;
    events_.pop(code);
    return code;
}

EventStream& Keyboard::events() {
    return stream_;
}

void Keyboard::grab_hook(void* /*context*/, bool grabbed) {
    Keyboard::self().glyphs_live_ = !grabbed;
}

}  // namespace cinux::driver
