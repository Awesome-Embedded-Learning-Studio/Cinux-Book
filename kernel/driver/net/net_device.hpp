/**
 * @file    net_device.hpp
 * @brief   The network-device face: frame words, a compile-time
 *          contract, and its erased form.
 *
 * A network device is any type satisfying NetworkDevice — a plain class
 * with six ordinary methods, checked at compile time, no inheritance,
 * no hand-written table, the same pair-shape the block family uses one
 * directory over. Consumers hold a NetPort and never learn which device
 * sits behind it; the loopback today, the e1000 card at a later
 * station. Two decisions shape the face. Whether an Ethernet header is
 * part of the wire is the device's own business — the loopback carries
 * none, a real card adds one — so send_l3 takes a layer-three payload
 * and lets the device frame it. And the destination travels as a
 * hardware address, so address resolution stays a protocol-stack
 * question that never reaches this seam.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <concepts>
#include <cstdint>

#include "kernel/driver/net/net_config.hpp"

namespace cinux::driver::net {

/**
 * @brief   One Ethernet address, as its own bytes.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct MacAddress {
    std::uint8_t bytes[kMacAddressBytes] = {};
};

/**
 * @brief   One EtherType word, the layer-three protocol a frame carries.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct EtherType {
    std::uint16_t value;
};

/**
 * @brief   One received frame and its true length.
 * @note    The buffer sizes to the family maximum; size says how much
 *          of it the device filled.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct RxFrame {
    std::uint8_t  bytes[kMaxEthernetFrame] = {};
    unsigned long size                     = 0;
};

/**
 * @brief   What a network device must offer, checked at compile time.
 * @tparam  Dev   The device type being checked.
 * @note    poll_frame hands out one received frame or answers false;
 *          send_l3 takes one layer-three payload and answers whether
 *          the device took it. A device without a hardware address —
 *          the loopback — answers false from has_mac, and max_frame is
 *          the device's own honest ceiling. Requirements grow the day
 *          a consumer needs them.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
template <typename Dev>
concept NetworkDevice = requires(Dev& device, const MacAddress& destination, EtherType type,
                                 const std::uint8_t* payload, unsigned long length, RxFrame& into) {
    { device.has_mac() } -> std::same_as<bool>;
    { device.mac() } -> std::same_as<MacAddress>;
    { device.has_ethernet_header() } -> std::same_as<bool>;
    { device.max_frame() } -> std::same_as<unsigned long>;
    { device.poll_frame(into) } -> std::same_as<bool>;
    { device.send_l3(destination, type, payload, length) } -> std::same_as<bool>;
};

/**
 * @brief   The erased device face: six plain entry points.
 * @note    Generated, never hand-written: EraseNetworkDevice builds the
 *          whole struct from a concept-checked type, so a new device
 *          adds zero boilerplate and the cast back to the real type
 *          lives in exactly one generated place per device.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
struct NetOps {
    /// Whether the device owns a hardware address.
    bool (*has_mac)(void* self);
    /// The device's hardware address, zeros when it keeps none.
    MacAddress (*mac)(void* self);
    /// Whether the device's wire carries an Ethernet header.
    bool (*has_ethernet_header)(void* self);
    /// The largest frame the device moves.
    unsigned long (*max_frame)(void* self);
    /// Hands one received frame out, or answers false.
    bool (*poll_frame)(void* self, RxFrame& into);
    /// Takes one layer-three payload for the wire, or refuses it.
    bool (*send_l3)(void* self, const MacAddress& destination, EtherType type,
                    const std::uint8_t* payload, unsigned long length);
};

/**
 * @brief         Builds the erased face for one concept-checked device.
 * @tparam        Dev   Device type satisfying NetworkDevice.
 * @return        The complete NetOps for that type.
 * @note          Every forwarder is captureless, so each decays to a
 *                plain function pointer; the device pointer rides along
 *                as the first argument at call time, never stored inside.
 * @since         0.6.0
 * @ingroup       kernel_driver
 */
template <NetworkDevice Dev>
[[nodiscard]] constexpr NetOps EraseNetworkDevice() {
    return {
        [](void* self) { return static_cast<Dev*>(self)->has_mac(); },
        [](void* self) { return static_cast<Dev*>(self)->mac(); },
        [](void* self) { return static_cast<Dev*>(self)->has_ethernet_header(); },
        [](void* self) { return static_cast<Dev*>(self)->max_frame(); },
        [](void* self, RxFrame& into) { return static_cast<Dev*>(self)->poll_frame(into); },
        [](void* self, const MacAddress& destination, EtherType type, const std::uint8_t* payload,
           unsigned long length) {
            return static_cast<Dev*>(self)->send_l3(destination, type, payload, length);
        },
    };
}

/**
 * @brief   One network device behind the face, ready to call.
 * @note    adopt() erases exactly once per device; the device's lifetime
 *          must cover every later call through the port. An unadopted
 *          port answers nothing: every entry stays null and the
 *          forwarders refuse the call.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
class NetPort {
public:
    NetPort() = default;

    /**
     * @brief         Seats one concept-checked device behind this port.
     *
     * @param[in,out] device  The device instance taking the seat.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    template <NetworkDevice Dev>
    void adopt(Dev& device) {
        static constexpr NetOps kErased = EraseNetworkDevice<Dev>();
        ops_                            = &kErased;
        self_                           = &device;
    }

    /**
     * @brief         Whether the seated device owns a hardware address.
     * @return        True for a device with one, false for an unadopted
     *                port or a device without.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool has_mac() const { return ops_ != nullptr && ops_->has_mac(self_); }

    /**
     * @brief         The seated device's hardware address.
     * @return        The address, zeros for an unadopted port or a
     *                device without one.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] MacAddress mac() const {
        return ops_ != nullptr ? ops_->mac(self_) : MacAddress{};
    }

    /**
     * @brief         Whether the seated device's wire carries an
     *                Ethernet header.
     * @return        True when it does, false for an unadopted port.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool has_ethernet_header() const {
        return ops_ != nullptr && ops_->has_ethernet_header(self_);
    }

    /**
     * @brief         The largest frame the seated device moves.
     * @return        The ceiling, zero for an unadopted port.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] unsigned long max_frame() const {
        return ops_ != nullptr ? ops_->max_frame(self_) : 0;
    }

    /**
     * @brief         Hands one received frame out.
     * @param[out]    into   The frame the device fills.
     * @return        True with a frame, false when none waits or the
     *                port is unadopted.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool poll_frame(RxFrame& into) const {
        return ops_ != nullptr && ops_->poll_frame(self_, into);
    }

    /**
     * @brief         Takes one layer-three payload for the wire.
     * @param[in]     destination  The hardware address to frame it for.
     * @param[in]     type         The layer-three protocol word.
     * @param[in]     payload      The payload bytes.
     * @param[in]     length       How many payload bytes.
     * @return        True when the device took the payload, false for a
     *                refusal or an unadopted port.
     * @since         0.6.0
     * @ingroup       kernel_driver
     */
    [[nodiscard]] bool send_l3(const MacAddress& destination, EtherType type,
                               const std::uint8_t* payload, unsigned long length) const {
        return ops_ != nullptr && ops_->send_l3(self_, destination, type, payload, length);
    }

private:
    const NetOps* ops_  = nullptr;
    void*         self_ = nullptr;
};

}  // namespace cinux::driver::net
