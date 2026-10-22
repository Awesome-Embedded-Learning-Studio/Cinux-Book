/**
 * @file    usb_keyboard.hpp
 * @brief   The USB boot keyboard: reports drained, edges translated.
 *
 * The class driver over the usb core: it holds one typed UsbDevice,
 * walks the descriptor chain to find its interrupt endpoint, and hands
 * the device a report handler that carries its own type — no untyped
 * word crosses the seam. The console terminal reads this device's
 * queue as its third input seat, after the serial line and the PS/2
 * port.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/container/ring_queue.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/usb/usb_config.hpp"
#include "kernel/driver/usb/usb_device.hpp"

namespace cinux::driver {

/**
 * @brief         Seats the USB keyboard in the device model when present.
 *
 * @return        True when one keyboard was seated, false when absent.
 * @note          Bring-up wiring for the device-registration step: the
 *                enumeration runs earlier, the seat happens where devfs
 *                is bound, alongside the other registered devices.
 * @warning       None
 * @throws        None
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool SeatUsbKeyboard();

/**
 * @brief   The USB boot keyboard this boot seated.
 * @note    The queue is the only state; the report shadows ride along
 *          so key edges survive across reports. Absent controller or
 *          failed enumeration leaves the device inert, never blocking.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class UsbKeyboard : public base::Singleton<UsbKeyboard> {
    friend class base::Singleton<UsbKeyboard>;

public:
    /**
     * @brief         Enumerates the keyboard through the seated controller.
     *
     * @return        True when one boot keyboard reports on its endpoint.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool init();

    /**
     * @brief         The slot this keyboard's device holds.
     *
     * @return        Slot id one or higher, zero when unseated.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint8_t seated_slot() const;

    /**
     * @brief         Drains finished reports, then answers the queue.
     *
     * @return        True when at least one glyph waits.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    bool poll();

    /**
     * @brief         Drains finished reports, then hands one glyph.
     *
     * @return        The oldest glyph, or 0 when nothing typed.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    char take();

private:
    UsbKeyboard() = default;

    static void report_arrived(UsbKeyboard& self, unsigned long status, unsigned long length);

    usb::UsbDevice<UsbKeyboard>                                 device_;
    base::container::RingQueue<char, usb::kUsbKeyQueueCapacity> events_{};
    unsigned char                                               previous_[8] = {};
};

}  // namespace cinux::driver
