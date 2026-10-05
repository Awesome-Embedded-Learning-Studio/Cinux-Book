/**
 * @file    keyboard.hpp
 * @brief   The PS/2 keyboard device: 8042 bring-up, the IRQ1 feed, one
 *          event queue drained by poll and take.
 *
 * The scancode grammar lives next door in scancode.hpp, the queue is the
 * base RingQueue; this header is only the device. Bring-up walks the
 * datasheet sequence, attach seats the IRQ1 handler with the interrupt
 * service, and on_byte feeds the translator from the data port —
 * integer-only, nothing slow, the IRQ path's standing rule.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/container/ring_queue.hpp"
#include "kernel/driver/keyboard_config.hpp"
#include "kernel/driver/scancode.hpp"

namespace cinux::driver {

/// The PS/2 keyboard device as a Meyers singleton.
class Keyboard {
public:
    /**
     * @brief     The one keyboard instance.
     *
     * @return    Reference to the Meyers singleton.
     * @since     0.1.0
     * @ingroup   kernel_driver
     */
    static Keyboard& self();

    /**
     * @brief         Walk the 8042 bring-up: both ports off, output
     *                flushed, config rewritten (IRQ1 on, IRQ12 off,
     *                translation on), self-test answered, first port
     *                back on.
     *
     * @return        True when the controller answered 0x55 to its
     *                self-test; false leaves the device unattached.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    bool init();

    /**
     * @brief         Seat the IRQ1 handler with the interrupt service
     *                and open the line.
     *
     * @return        None
     * @note          Bring-up order is caller's: the interrupt service
     *                must already hold its chip.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void attach();

    /**
     * @brief         Feed one raw byte from the data port through the
     *                translator; a printable result joins the queue.
     *
     * @param[in]     raw_byte   Byte read from port 0x60.
     * @return        None
     * @note          Runs on the IRQ1 path: integer-only, queue push,
     *                nothing slow.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    void on_byte(uint8_t raw_byte);

    /// Whether at least one event waits.
    [[nodiscard]] bool poll() const;

    /**
     * @brief         Take the oldest event.
     *
     * @return        The event's character.
     * @warning       Undefined when poll() is false; callers gate on
     *                poll.
     * @since         0.1.0
     * @ingroup       kernel_driver
     */
    char take();

private:
    Keyboard() = default;

    TranslatorState                                          state_{};
    base::container::RingQueue<char, kKeyboardEventCapacity> events_{};
};

}  // namespace cinux::driver
