#include "kernel/driver/mouse/ps2_mouse.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/driver/mouse/mouse_packet.hpp"
#include "kernel/driver/ps2/controller.hpp"
#include "kernel/interrupt/irq.hpp"

namespace {
constexpr uint16_t kEvRel     = 2;
constexpr uint16_t kRelX      = 0;
constexpr uint16_t kRelY      = 1;
constexpr uint16_t kBtnLeft   = 0x110;
constexpr uint16_t kBtnRight  = 0x111;
constexpr uint16_t kBtnMiddle = 0x112;

constexpr uint8_t kAuxDefaults      = 0xF6;
constexpr uint8_t kAuxEnableReports = 0xF4;

constexpr unsigned int kMouseIrqLineValue = 12;

bool arm_irq12(cinux::driver::ps2::Ps2Ports ports) {
    if (!cinux::driver::ps2::SendCommand(ports, cinux::driver::ps2::kCommandReadConfig)) {
        return false;
    }
    uint8_t config = 0;
    if (!cinux::driver::ps2::ReadWhenReady(ports, config)) {
        return false;
    }
    cinux::base::bit::BitMask<uint8_t> wanted{config};
    wanted = wanted | cinux::base::bit::BitMask<uint8_t>{cinux::driver::ps2::kConfigIrqSecond};
    return cinux::driver::ps2::SendCommand(ports, cinux::driver::ps2::kCommandWriteConfig) &&
           cinux::driver::ps2::WriteData(ports, wanted.raw);
}

void irq12_thunk() {
    cinux::driver::Ps2Mouse::self().on_byte(cinux::driver::InB(cinux::driver::ps2::kDataPort));
}

}  // namespace

namespace cinux::driver {

bool Ps2Mouse::init() {
    const arch::IrqGuard               kQuiet;
    const cinux::driver::ps2::Ps2Ports kPorts = ps2::HostPs2Ports();
    if (!ps2::SendCommand(kPorts, ps2::kCommandEnableSecond)) {
        return false;
    }
    if (!ps2::SendAux(kPorts, kAuxDefaults)) {
        return false;
    }
    if (!ps2::SendAux(kPorts, kAuxEnableReports)) {
        return false;
    }
    if (!arm_irq12(kPorts)) {
        return false;
    }
    cinux::interrupt::Irq::self().register_handler(
        cinux::interrupt::IrqLine{.value = kMouseIrqLineValue}, irq12_thunk);
    cinux::interrupt::Irq::self().enable_line(
        cinux::interrupt::IrqLine{.value = kMouseIrqLineValue});
    return true;
}

void Ps2Mouse::on_byte(uint8_t raw_byte) {
    MouseSample sample{};
    if (!grammar_.feed(raw_byte, sample)) {
        return;
    }
    if (sample.dx != 0) {
        stream_.publish(kEvRel, kRelX, sample.dx);
    }
    if (sample.dy != 0) {
        stream_.publish(kEvRel, kRelY, sample.dy);
    }
    if (sample.left != left_) {
        left_ = sample.left;
        stream_.publish(kEvKey, kBtnLeft, left_ ? 1 : 0);
    }
    if (sample.right != right_) {
        right_ = sample.right;
        stream_.publish(kEvKey, kBtnRight, right_ ? 1 : 0);
    }
    if (sample.middle != middle_) {
        middle_ = sample.middle;
        stream_.publish(kEvKey, kBtnMiddle, middle_ ? 1 : 0);
    }
    stream_.publish_report();
}

EventStream& Ps2Mouse::events() {
    return stream_;
}

}  // namespace cinux::driver
