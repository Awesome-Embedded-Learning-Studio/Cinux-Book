/**
 * @file    mouse_packet.hpp
 * @brief   Pure set-1 auxiliary packet decoding, independent of the
 *          device lifecycle.
 *
 * @author  Charliechen114514
 * @date    2026-10-28
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once
#include <stdint.h>
namespace cinux::driver {
/// One decoded report: motion the screen understands and the button
/// state the packet carried.
struct MouseSample {
    int32_t dx;      ///< Horizontal motion, positive toward the right.
    int32_t dy;      ///< Vertical motion, positive toward the bottom.
    bool    left;    ///< Left button held.
    bool    right;   ///< Right button held.
    bool    middle;  ///< Middle button held.
};

/// The packet grammar: bytes in, one sample per three-byte report.
class MousePacketDecoder {
public:
    MousePacketDecoder() = default;

    /**
     * @brief         Offers one byte to the grammar.
     *
     * @param[in]     raw_byte   Byte read from the data port.
     * @param[out]    out        The finished report, when this byte
     *                          closed one.
     * @return        True when out carries a report.
     * @note          A byte that cannot be a packet head while the
     *                decoder hunts for one is dropped, not buffered —
     *                the always-one bit and the clear overflow bits
     *                are the head's signature, and a stream that lost
     *                its way resynchronizes on the next head.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool feed(uint8_t raw_byte, MouseSample& out);

private:
    uint8_t bytes_[3] = {};
    uint8_t seated_   = 0;
};

}
