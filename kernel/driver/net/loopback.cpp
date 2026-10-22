/**
 * @file    loopback.cpp
 * @brief   The loopback's queue, filled in order and drained in order.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/net/loopback.hpp"

#include <cstdint>

#include "cinux/memory.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"

namespace cinux::driver::net {

void Loopback::init() {
    RxFrame drained{};
    while (queue_.pop(drained)) {
    }
}

bool Loopback::has_mac() const {
    return false;
}

MacAddress Loopback::mac() const {
    return MacAddress{};
}

bool Loopback::has_ethernet_header() const {
    return false;
}

unsigned long Loopback::max_frame() const {
    return kMaxEthernetFrame;
}

bool Loopback::poll_frame(RxFrame& into) {
    return queue_.pop(into);
}

bool Loopback::send_l3(const MacAddress& /*destination*/, EtherType /*type*/,
                       const std::uint8_t* payload, unsigned long length) {
    if (payload == nullptr || length == 0 || length > kMaxEthernetFrame) {
        return false;
    }
    RxFrame frame{};
    base::CopyBytes(frame.bytes, payload, length);
    frame.size = length;
    return queue_.push(frame);
}

}  // namespace cinux::driver::net
