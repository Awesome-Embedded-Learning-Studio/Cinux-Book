/**
 * @file    ps2_mouse.hpp
 * @brief   The PS/2 auxiliary pointer: packet grammar, 8042 second
 *          port bring-up, and the IRQ12 feed.
 *
 * The device speaks three-byte packets on the controller's second port,
 * one packet per report, buttons and motion in a fixed layout. The
 * decoder here is pure grammar — host-testable, no ports — while the
 * singleton owns the bring-up: enable the port, set the device's
 * defaults, start its reporting, and only then open the IRQ12 line, so
 * the acknowledgement bytes the device answers with arrive before any
 * handler could misread them.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/singleton.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/mouse/mouse_packet.hpp"

namespace cinux::driver {

/// The PS/2 pointer device as a Meyers singleton.
class Ps2Mouse : public cinux::base::Singleton<Ps2Mouse> {
    friend class cinux::base::Singleton<Ps2Mouse>;

public:
    /**
     * @brief         Walks the auxiliary bring-up: port on, defaults
     *                set, reporting started, IRQ12 armed.
     *
     * @return        True when the device acknowledged every command
     *                and the line opened; false leaves the device
     *                inert, never blocking the boot.
     * @note          The keyboard's bring-up must already have walked
     *                the controller's self-test, and IRQ12 stays
     *                closed until the device has answered, so the
     *                acknowledgement bytes meet no handler.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool init();

    /**
     * @brief         Feed one byte from the data port through the
     *                grammar; a finished report becomes event words.
     *
     * @param[in]     raw_byte   Byte read from port 0x60.
     * @return        None
     * @note          Runs on the IRQ12 path: integer-only, queue push,
     *                nothing slow.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    void on_byte(uint8_t raw_byte);

    /**
     * @brief         The pointer's event stream, the input node's
     *                device half.
     *
     * @return        The stream this device publishes into.
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] EventStream& events();

private:
    Ps2Mouse() = default;

    MousePacketDecoder grammar_;
    EventStream        stream_;
    bool               left_   = false;
    bool               right_  = false;
    bool               middle_ = false;
};

}  // namespace cinux::driver
