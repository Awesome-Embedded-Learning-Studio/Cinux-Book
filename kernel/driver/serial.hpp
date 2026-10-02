/**
 * @file    serial.hpp
 * @brief   The 16550 UART driver: two functions, everything else private.
 *
 * The kernel-side serial driver. Registers, the init table, and the polling
 loop live in the .cpp; the public face is exactly "start it" and "send
 one byte".
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
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

}  // namespace cinux::driver
