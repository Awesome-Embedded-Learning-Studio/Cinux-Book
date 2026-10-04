/**
 * @file    pit.hpp
 * @brief   The 8253/8254 PIT as a tick backend.
 *
 * A singleton whose chip keeps all state: the kernel only needs to know
 * how to twist three knobs — command byte, divisor low, divisor high.
 * Everything below the single start face (input clock, ports, divisor
 * arithmetic) is a device fact and lives in the .cpp, invisible to
 * consumers. Satisfies time::TickSource without knowing that contract
 * exists.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"

namespace cinux::driver {

/**
 * @brief         Programmable Interval Timer, channel 0.
 * @note          Meyers-singleton shape after Pmm and Tick; the chip
 *                holds every bit of state, so the instance carries none
 *                and the zero-construction rule holds.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
class Pit {
public:
    /**
     * @brief         The one timer device.
     *
     * @return        Reference to the PIT instance.
     * @since         0.2.0
     * @ingroup       kernel_driver
     */
    static Pit& self();

    /**
     * @brief         Programs channel 0 as a square wave at the given rate.
     *
     * @param[in]     rate   Desired oscillation; divisor is
     *                      kPitInputHz / rate.value and must fit 16 bits.
     * @return        None
     * @note          Three port writes in one table: 0x36 to the command
     *                      port, then low and high divisor bytes.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void start(cinux::base::Hertz rate);

private:
    Pit() = default;
};

}  // namespace cinux::driver
