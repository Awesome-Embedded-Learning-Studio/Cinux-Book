/**
 * @file    xhci.hpp
 * @brief   The xHCI host controller: bring-up, rings, and the drain.
 *
 * Meyers-singleton shape after Ahci. This layer owns the controller
 * and nothing above it: the register window, the reset dance, the
 * command and event rings, and the delivery of completions — polled by
 * readers, and armed through MSI-X for the day real interrupts arrive,
 * the v1 boundary kept honestly: under this hypervisor delivery is
 * unreliable and the drain does the carrying. Seated devices live one
 * layer up in usb_device and receive their transfer events here.
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

#include "cinux/singleton.hpp"
#include "kernel/driver/usb/usb_config.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"

namespace cinux::driver::usb {

template <typename Owner>
class UsbDevice;

/// @brief One seated device as the event drain sees it, erased at the seam.
struct DeviceRoute {
    void* device;                               ///< The seated device, opaque here.
    void (*deliver)(void* device, const Trb&);  ///< Hands one transfer event over.
};

}  // namespace cinux::driver::usb

namespace cinux::driver {

struct PciDevice;

/**
 * @brief   The USB host controller this machine carries.
 * @note    All state is controller state: window, regions, rings and
 *          the seated-device pointers events route to. None of it
 *          changes shape after init, so the zero-construction rule
 *          holds.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class XhciHost : public base::Singleton<XhciHost> {
    friend class base::Singleton<XhciHost>;
    template <typename Owner>
    friend class usb::UsbDevice;

public:
    /**
     * @brief         Finds the controller and walks the bring-up sequence.
     *
     * @return        True when the controller runs; false when absent or
     *                stalled, in which case nothing stays mapped.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool init();

    /**
     * @brief         Whether a controller was brought up.
     *
     * @return        True after a successful init on this boot.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool present() const;

    /**
     * @brief         Root-hub ports the controller offers.
     *
     * @return        Port count as the capability region stated.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned int max_ports() const;

    /**
     * @brief         Drains every finished event on the ring.
     *
     * @return        None
     * @note          Command completions land in the waiter that asked,
     *                transfer events route to the seated device that
     *                owns the TRB. Readers poll this, the armed message
     *                vector calls it, and both carry the same cargo.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    void drain_events();

    /**
     * @brief         Whether the controller reported a host system error.
     *
     * @return        False while the controller is healthy.
     * @warning       None
     * @throws        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool unhealthy() const;

private:
    XhciHost() = default;

    [[nodiscard]] uint32_t read32(unsigned long offset) const;
    void                   write32(unsigned long offset, uint32_t value);
    void                   write64(unsigned long offset, uint64_t value);
    [[nodiscard]] bool     wait_set(unsigned long offset, uint32_t mask);
    [[nodiscard]] bool     wait_clear(unsigned long offset, uint32_t mask);
    [[nodiscard]] bool     halt_and_reset();
    [[nodiscard]] bool     arm_rings();
    [[nodiscard]] bool     start_controller();
    [[nodiscard]] bool     run_command(uint64_t parameter, uint32_t control, uint8_t& slot);
    void                   write_doorbell(unsigned int slot, unsigned int target);
    [[nodiscard]] uint32_t port_status(unsigned int port) const;
    [[nodiscard]] bool     reset_port(unsigned int port, uint8_t& speed);
    void                   seat_device(uint8_t slot, void* device,
                                       void (*deliver)(void* device, const usb::Trb& event));
    void                   consume_event();
    [[nodiscard]] bool     await_command(uint64_t trb_address, uint8_t& slot, uint8_t& code);
    void                   arm_delivery(const PciDevice* host);

    volatile unsigned char* window_ = nullptr;
    usb::XhciRegions        regions_{};
    usb::XhciShape          shape_{};
    usb::RingState          command_{};
    unsigned long           event_index_                    = 0;
    bool                    event_cycle_                    = true;
    uint64_t                command_page_                   = 0;
    uint64_t                event_page_                     = 0;
    uint64_t                device_page_                    = 0;
    usb::DeviceRoute        routes_[usb::kXhciSlotsEnabled] = {};
    uint64_t                awaited_trb_                    = 0;
    bool                    command_ready_                  = false;
    uint8_t                 command_slot_                   = 0;
    uint8_t                 command_code_                   = 0;
};

}  // namespace cinux::driver
