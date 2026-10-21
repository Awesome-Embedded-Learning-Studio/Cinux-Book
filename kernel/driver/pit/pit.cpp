/**
 * @file    pit.cpp
 * @brief   Device facts of the PIT: input clock, ports, divisor math.
 *
 * The 1.193182 MHz input clock is a property of the chip wired to the
 * board, not a knob of the tick service — it stays here, out of every
 * config header.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/pit/pit.hpp"

#include <stdint.h>

#include "cinux/assert.hpp"
#include "cinux/bit_ops/bit_ops.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/math.hpp"
#include "cinux/time.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/driver/base/io.hpp"

namespace {

constexpr unsigned long long kPitInputHz = 1193182;

constexpr unsigned short kCommandPort      = 0x43;
constexpr unsigned short kChannel0DataPort = 0x40;

constexpr uint8_t kSquareWaveCommand = 0x36;

}  // namespace

namespace cinux::driver {

void Pit::wait_microseconds(base::time::Microseconds span) {
    base::safety::Check(span.value != 0 && span.value <= 50000, "PIT delay exceeds channel span");
    constexpr unsigned short kChannelTwoPort    = 0x42;
    constexpr unsigned short kSpeakerPort       = 0x61;
    constexpr uint8_t        kChannelTwoOneShot = 0xB0;
    constexpr auto           kGate              = base::bit::MaskBit<uint32_t>(0);
    constexpr auto           kSpeaker           = base::bit::MaskBit<uint32_t>(1);
    constexpr auto           kDone              = base::bit::MaskBit<uint32_t>(5);
    const uint8_t            kOriginal          = InB(kSpeakerPort);
    const auto kClosed = static_cast<uint8_t>(kOriginal & ~(kGate.raw | kSpeaker.raw));
    const auto kCount  = static_cast<uint16_t>(base::math::Ceil(kPitInputHz * span.value, 1000000));
    OutB(PortWrite{.port = kSpeakerPort, .value = kClosed});
    const PortWrite kProgram[] = {
        {.port = kCommandPort, .value = kChannelTwoOneShot},
        {.port = kChannelTwoPort, .value = static_cast<uint8_t>(base::bit::LowByte(kCount))},
        {.port = kChannelTwoPort, .value = static_cast<uint8_t>(base::bit::HighByte(kCount))},
        {.port = kSpeakerPort, .value = static_cast<uint8_t>(kClosed | kGate.raw)},
    };
    OutB(kProgram);
    while (!base::bit::BitMask<uint32_t>{InB(kSpeakerPort)}.has(kDone)) {
        arch::CpuRelax();
    }
    OutB(PortWrite{.port = kSpeakerPort, .value = kOriginal});
}

void Pit::start(cinux::base::Hertz rate) {
    const auto kDivisor = static_cast<unsigned short>(kPitInputHz / rate.value);

    const cinux::driver::PortWrite kWrites[] = {
        {.port = kCommandPort, .value = kSquareWaveCommand},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor & 0xFFU)},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor >> 8U)},
    };
    OutB(kWrites);
}

}  // namespace cinux::driver
