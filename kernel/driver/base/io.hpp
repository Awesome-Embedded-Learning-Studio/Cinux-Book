/**
 * @file    io.hpp
 * @brief   Port I/O primitives with a semantic face.
 *
 * Writes travel as address-value pairs and polls wait on named bits; the raw
 two-argument forms live in the .cpp so nobody reaches around the
 vocabulary.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver {

/**
 * @brief   One port write as data: where and what.
 * @note    Init sequences become tables of these, and a table is a
 *          datasheet you can read.
 * @since   0.1.0
 * @ingroup kernel_driver
 */
struct PortWrite {
    uint16_t port;
    uint8_t  value;
};

/**
 * @brief         Performs one port write described by the pair.
 *
 * @param[in]     write   Port and value to emit.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void OutB(PortWrite write);

/**
 * @brief         Performs a whole table of port writes in order.
 *
 * @tparam        Count   Table length, deduced from the array.
 * @param[in]     writes  Register writes to apply front to back.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
template <unsigned long Count>
void OutB(const PortWrite (&writes)[Count]) {
    for (unsigned long index = 0; index < Count; ++index) {
        OutB(writes[index]);
    }
}

/**
 * @brief         Reads one byte from a port.
 *
 * @param[in]     port   Port to read.
 * @return        The byte the device answered.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
uint8_t InB(uint16_t port);

/**
 * @brief         Spins until every bit of mask reads one.
 *
 * @param[in]     port   Status port to poll.
 * @param[in]     mask   Bits that must all be set to continue.
 * @return        None
 * @note          The transmitter-ready wait of every polling driver.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void WaitBitsSet(uint16_t port, uint8_t mask);

/**
 * @brief         Spins until every bit of mask reads zero.
 *
 * @param[in]     port   Status port to poll.
 * @param[in]     mask   Bits that must all be clear to continue.
 * @return        None
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
void WaitBitsClear(uint16_t port, uint8_t mask);

}  // namespace cinux::driver
