/**
 * @file    serial.hpp
 * @brief   The 16550 UART driver: start, send, and see the line drain.
 *
 * The kernel-side serial driver. Registers, the init table, and the polling
 loop live in the .cpp; the public face is "start it", "send one byte",
 and "wait until every sent byte has left the wire" — the last one
 before anything as final as power off.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver {

/**
 * @brief         Programs COM1 for polling output.
 *
 * @return        None
 * @note          Idempotent: 115200 8N1, FIFOs on, interrupts off.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void SerialInit();

/**
 * @brief         Sends one byte, waiting until the line can take it.
 *
 * @param[in]     character   Byte to transmit.
 * @return        None
 * @note          Blocks on the transmitter-hold-empty bit.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void SerialPutChar(char character);

/**
 * @brief         Waits until the transmitter has fully emptied.
 *
 * @return        None
 * @note          Sending only waits for the hold register; the byte
 *                then spends its shift-register time on the wire. This
 *                waits for both to be empty, so whatever was printed
 *                has truly left the machine — the call that belongs
 *                right before cutting the power.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup kernel_driver
 */
void SerialDrain();

/**
 * @brief         Takes one received byte when the line holds one.
 *
 * @param[out]    out   Receives the byte, untouched when none waits.
 * @return        True when a byte was taken.
 * @note          The receiver's half of the line, polled: the
 *                data-ready bit says whether the hold register holds a
 *                byte, and reading the register consumes it. No
 *                interrupt arms the arrival — callers poll between
 *                other work.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup kernel_driver
 */
bool SerialTryGetChar(char* out);

/**
 * @brief         Reports whether the line holds a received byte.
 *
 * @return        True when the data-ready bit is set.
 * @note          The peek face for readiness: nothing is consumed, the
 *                byte stays for the next SerialTryGetChar.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_driver
 */
bool SerialHasKey();

}  // namespace cinux::driver
