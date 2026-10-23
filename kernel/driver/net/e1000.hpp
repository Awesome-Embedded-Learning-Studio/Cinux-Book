/**
 * @file    e1000.hpp
 * @brief   The 82540em network card: the family's first wire.
 *
 * A polling driver by verdict of the emulator: QEMU's device model
 * only advances while the guest traps out, so the card never raises an
 * interrupt this driver would trust, and delivery waits for the pump's
 * next visit instead. poll_frame's first act is reading the receive
 * head over MMIO — the read itself is the doorbell that lets the
 * emulator run the card — and send_l3 frames the payload with the kernel's own
 * address and hands a descriptor to the transmit ring. The card is
 * claimed by vendor and device identity, never by class: a class match
 * would also bind siblings with different register maps.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include <cstdint>

#include "cinux/addr.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/net/e1000_config.hpp"
#include "kernel/driver/net/net_device.hpp"

namespace cinux::driver::net {

/**
 * @brief   The one e1000 card, satisfying NetworkDevice.
 * @note    A singleton like the loopback: one card, one seat, the boot
 *          wiring decides whether it joins the stack. Ring storage
 *          comes from the physical allocator and stays owned for the
 *          machine's lifetime.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class E1000 : public cinux::base::Singleton<E1000> {
    friend class cinux::base::Singleton<E1000>;

public:
    /**
     * @brief         Finds and brings the card up, once per boot.
     *
     * @return        True when the card answered and the rings are live.
     * @note          Identity match, window mapped, reset, interrupts
     *                masked, MAC read from the EEPROM, both rings armed.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    bool init();

    /**
     * @brief         Whether the card joined this boot.
     * @return        True after a successful init.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool present() const;

    /**
     * @brief         Where the card has consumed the transmit ring to.
     * @return        The transmit head index.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long tx_head() const;

    /**
     * @brief         Where the card has filled the receive ring to.
     * @return        The receive head index.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long rx_head() const;

    /**
     * @brief         This card's seat for stack wiring.
     * @return        The erased port adopting this card.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] NetPort& port();

    /**
     * @brief         Whether this device owns a hardware address.
     * @return        True, the EEPROM read one.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool has_mac() const;

    /**
     * @brief         This card's hardware address.
     * @return        The six bytes the EEPROM spelled.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] MacAddress mac() const;

    /**
     * @brief         Whether this device's wire carries an Ethernet header.
     * @return        True; this device frames everything it moves.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool has_ethernet_header() const;

    /**
     * @brief         The largest frame this device moves.
     * @return        The family maximum.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long max_frame() const;

    /**
     * @brief         Hands one received frame out.
     *
     * @param[out]    into   The frame the card filled.
     * @return        True with a frame; false when the rings are quiet.
     * @note          The receive-head read at the top is deliberate:
     *                it is the MMIO touch that steps the emulator's
     *                device model, the family's first lesson.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool poll_frame(RxFrame& into);

    /**
     * @brief         Frames and sends one layer-three payload.
     *
     * @param[in]     destination  Hardware address to frame for.
     * @param[in]     type         Ethertype word for the payload.
     * @param[in]     payload      First payload byte.
     * @param[in]     length       Payload bytes.
     * @return        True when a descriptor took the frame.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool send_l3(const MacAddress& destination, EtherType type,
                               const std::uint8_t* payload, unsigned long length);

private:
    E1000() = default;

    void read_mac();

    unsigned long regs_    = 0;
    bool          present_ = false;
    MacAddress    mac_     = {};

    base::PhysAddr ring_page_;
    base::PhysAddr rx_buffers_[kE1000RxRingSize];
    base::PhysAddr tx_buffers_[kE1000TxRingSize];
    unsigned long  rx_tail_ = 0;
    unsigned long  tx_next_ = 0;
    NetPort        seat_;
};

}  // namespace cinux::driver::net
