/**
 * @file    pic.cpp
 * @brief   Port sequences of the 8259 pair.
 *
 * Part of the no-SSE interrupt island: the service's dispatch reaches
 * ack on every interrupt, and everything on the iret path keeps off
 * the vector registers. Each chip's setup is one PortWrite table — an
 * initialization-command-word sequence you can read like a datasheet
 * row.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#include "kernel/arch/x86_64/pic.hpp"

#include <stdint.h>

#include "kernel/driver/base/io.hpp"
#include "kernel/interrupt/irq.hpp"

namespace {

constexpr uint16_t kMasterCommandPort = 0x20;
constexpr uint16_t kMasterDataPort    = 0x21;
constexpr uint16_t kSlaveCommandPort  = 0xA0;
constexpr uint16_t kSlaveDataPort     = 0xA1;

constexpr uint8_t kIcw1Init    = 0x11;
constexpr uint8_t kIcw4x86Mode = 0x01;
constexpr uint8_t kMaskAll     = 0xFF;
constexpr uint8_t kAcknowledge = 0x20;

constexpr uint8_t kMasterVectorBase = 0x20;
constexpr uint8_t kSlaveVectorBase  = 0x28;
constexpr uint8_t kSlaveOnIrq2      = 0x04;
constexpr uint8_t kSlaveIdentity2   = 0x02;

constexpr unsigned int kFirstSlaveLine = 8;

const cinux::driver::PortWrite kMasterSetup[] = {
    {.port = kMasterCommandPort, .value = kIcw1Init},
    {.port = kMasterDataPort, .value = kMasterVectorBase},
    {.port = kMasterDataPort, .value = kSlaveOnIrq2},
    {.port = kMasterDataPort, .value = kIcw4x86Mode},
    {.port = kMasterDataPort, .value = kMaskAll},
};

const cinux::driver::PortWrite kSlaveSetup[] = {
    {.port = kSlaveCommandPort, .value = kIcw1Init},
    {.port = kSlaveDataPort, .value = kSlaveVectorBase},
    {.port = kSlaveDataPort, .value = kSlaveIdentity2},
    {.port = kSlaveDataPort, .value = kIcw4x86Mode},
    {.port = kSlaveDataPort, .value = kMaskAll},
};

}  // namespace

namespace cinux::arch {

void Pic::remap() {
    cinux::driver::OutB(kMasterSetup);
    cinux::driver::OutB(kSlaveSetup);
}

void Pic::unmask(cinux::interrupt::IrqLine line) {
    if (line.value < kFirstSlaveLine) {
        const uint8_t kMask = cinux::driver::InB(kMasterDataPort);
        cinux::driver::OutB(cinux::driver::PortWrite{
            .port = kMasterDataPort, .value = static_cast<uint8_t>(kMask & ~(1U << line.value))});
        return;
    }
    const uint8_t kSlaveMask = cinux::driver::InB(kSlaveDataPort);
    cinux::driver::OutB(cinux::driver::PortWrite{
        .port  = kSlaveDataPort,
        .value = static_cast<uint8_t>(kSlaveMask & ~(1U << (line.value - kFirstSlaveLine)))});
    const uint8_t kMasterMask = cinux::driver::InB(kMasterDataPort);
    cinux::driver::OutB(cinux::driver::PortWrite{
        .port = kMasterDataPort, .value = static_cast<uint8_t>(kMasterMask & ~kSlaveOnIrq2)});
}

void Pic::ack(cinux::interrupt::IrqLine line) {
    if (line.value >= kFirstSlaveLine) {
        cinux::driver::OutB(
            cinux::driver::PortWrite{.port = kSlaveCommandPort, .value = kAcknowledge});
    }
    cinux::driver::OutB(
        cinux::driver::PortWrite{.port = kMasterCommandPort, .value = kAcknowledge});
}

}  // namespace cinux::arch
