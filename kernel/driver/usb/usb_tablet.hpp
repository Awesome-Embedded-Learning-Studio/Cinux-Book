/**
 * @file    usb_tablet.hpp
 * @brief   The USB absolute pointer: tablet reports on absolute axes.
 *
 * The class driver over the usb core for an absolute pointing device:
 * it holds one typed UsbDevice, walks the descriptor chain for the HID
 * interface that speaks its raw report protocol, and publishes each
 * report's coordinates as absolute axes plus button edges. The event
 * stream carries a poll hook, so a reader's read drains the controller
 * and the next report lands without an interrupt line.
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

#include "cinux/singleton.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/usb/usb_device.hpp"

namespace cinux::driver {

/**
 * @brief   One decoded tablet report: button bits and both coordinates.
 * @since   0.9.0
 * @ingroup kernel_driver
 */
struct TabletReport {
    uint8_t  buttons;  ///< The report's button bits, three of them read.
    uint16_t x;        ///< The absolute x coordinate, zero based.
    uint16_t y;        ///< The absolute y coordinate, zero based.
};

/**
 * @brief         Decodes one tablet report from the wire bytes.
 *
 * @param[in]     report   The report buffer the endpoint filled.
 * @return        Button bits and both coordinates, little endian words.
 * @warning       None
 * @throws        None
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] TabletReport DecodeTablet(const unsigned char* report);

/**
 * @brief   The USB tablet this boot seated.
 * @note    The event stream is the only public face: absolute axes,
 *          button edges, one report word each. Failed enumeration
 *          leaves the device inert, never blocking the boot.
 * @since   0.9.0
 * @ingroup kernel_driver
 */
class UsbTablet : public base::Singleton<UsbTablet> {
    friend class base::Singleton<UsbTablet>;

public:
    /**
     * @brief         Enumerates the tablet through the seated controller.
     *
     * @return        True when one tablet reports on its endpoint.
     * @warning       None
     * @throws        None
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool init();

    /**
     * @brief         The tablet's event stream, axes and buttons.
     *
     * @return        The stream the report handler publishes into.
     * @warning       None
     * @throws        None
     * @since         0.9.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] EventStream& events();

private:
    UsbTablet() = default;

    static void report_arrived(UsbTablet& self, unsigned long status, unsigned long length);

    usb::UsbDevice<UsbTablet> device_;
    EventStream               stream_;
    uint8_t                   buttons_ = 0;
    bool                      ready_   = false;
};

}
