/**
 * @file    usb_device.hpp
 * @brief   One seated USB device, typed on the class driver it serves.
 *
 * The middle layer of the USB stack. The host controller below owns
 * bring-up and the event ring's drain; the class driver above holds
 * one UsbDevice and speaks transfers to it. A device owns its slot's
 * input and output context pages, its endpoint rings, and one armed
 * AsyncCallback per outstanding transfer — completions arrive from the
 * host's drain, match the transfer's TRB, and fire once, straight
 * into the class driver's own type.
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

#include "cinux/async.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"

namespace cinux::driver {

class XhciHost;

namespace usb {

/**
 * @brief   One addressed USB device on one seated slot.
 * @note    The class driver's type rides along: the report callback is
 *          typed on it, so no untyped word crosses this layer. One
 *          interrupt-in endpoint rides along too, the shape every boot
 *          device this station carries needs.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
template <typename Owner>
class UsbDevice {
public:
    /// What the class driver hands over for report completions.
    using ReportHandler = base::AsyncCallback<Owner>::Handler;

    /**
     * @brief         Takes a free slot and the device's bus address.
     *
     * @param[in,out] host   The controller that owns the slot.
     * @return        True when the slot is seated and addressed.
     * @note          The whole v1 sequence: port scan and reset, input
     *                context with slot and endpoint zero, Address
     *                Device. The device registers itself with the host
     *                so transfer events route back here.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool address(XhciHost& host);

    /**
     * @brief         One control read over endpoint zero.
     *
     * @param[in,out] host     The controller that owns the rings.
     * @param[in]     value    The descriptor word, wValue on the wire.
     * @param[out]    data     Destination buffer, copied on success.
     * @param[in]     length   How many bytes were asked for.
     * @return        True when the three stages completed cleanly.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool control_read(XhciHost& host, uint16_t value, void* data, uint16_t length);

    /**
     * @brief         One configuration write over endpoint zero.
     *
     * @param[in,out] host           The controller that owns the rings.
     * @param[in]     config_value   Which configuration to enter.
     * @return        True when the device accepted it.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool set_configuration(XhciHost& host, uint8_t config_value);

    /**
     * @brief         Seats the interrupt-in endpoint's context.
     *
     * @param[in,out] host         The controller that owns the rings.
     * @param[in]     max_packet   Report bytes the endpoint carries.
     * @param[in]     interval     The endpoint descriptor's bInterval.
     * @return        True when the controller seated the endpoint.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool configure_interrupt_in(XhciHost& host, uint16_t max_packet,
                                              uint8_t interval);

    /**
     * @brief         Arms one interrupt read with a completion callback.
     *
     * @param[in,out] host      The controller that owns the rings.
     * @param[in]     handler   The word to call when the report lands.
     * @param[in]     owner     The class driver the handler serves.
     * @return        None
     * @note          The report rides the device's own buffer; the
     *                handler reads it through report(). Completion
     *                consumes this submission and disarms its callback.
     *                The handler may submit the next read after it has
     *                consumed the buffer, since the next DMA reuses it.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    void interrupt_read(XhciHost& host, ReportHandler handler, Owner* owner);

    /**
     * @brief         The report buffer the interrupt endpoint fills.
     *
     * @return        Read-only view of the eight report bytes.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] const unsigned char* report() const;

    /**
     * @brief         The slot this device holds, zero before address.
     *
     * @return        Slot id one or higher, zero when unseated.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] uint8_t slot() const;

private:
    friend class ::cinux::driver::XhciHost;

    void on_transfer(const Trb& event);

    static void deliver(void* device, const Trb& event);

    void               ring_report(XhciHost& host);
    [[nodiscard]] bool spin_for_control(XhciHost& host);

    uint8_t                    slot_        = 0;
    uint8_t                    speed_       = 0;
    uint8_t                    port_        = 0;
    uint64_t                   input_page_  = 0;
    uint64_t                   output_page_ = 0;
    uint64_t                   ep0_page_    = 0;
    uint64_t                   ep1_page_    = 0;
    RingState                  ep0_{};
    RingState                  ep1_{};
    uint64_t                   control_trb_  = 0;
    bool                       control_done_ = false;
    uint8_t                    control_code_ = 0;
    uint64_t                   report_trb_   = 0;
    bool                       report_live_  = false;
    base::AsyncCallback<Owner> report_call_;
};

}  // namespace usb

}  // namespace cinux::driver
