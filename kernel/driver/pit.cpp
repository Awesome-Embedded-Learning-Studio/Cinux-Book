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

#include "kernel/driver/pit.hpp"

#include <stdint.h>

#include "cinux/literal_types.hpp"
#include "kernel/driver/base/io.hpp"

namespace {

constexpr unsigned long long kPitInputHz = 1193182;

constexpr unsigned short kCommandPort      = 0x43;
constexpr unsigned short kChannel0DataPort = 0x40;

constexpr uint8_t kSquareWaveCommand = 0x36;

}  // namespace

namespace cinux::driver {

Pit& Pit::self() {
    static Pit local_pit;
    return local_pit;
}

void Pit::start(cinux::base::Hertz rate) {
    const auto kDivisor = static_cast<unsigned short>(kPitInputHz / rate.value);

    const cinux::driver::PortWrite kWrites[] = {
        {.port = kCommandPort, .value = kSquareWaveCommand},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor & 0xFF)},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor >> 8)},
    };
    OutB(kWrites);
}

}  // namespace cinux::driver
