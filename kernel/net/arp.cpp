#include "kernel/net/arp.hpp"

#include <stddef.h>
#include <stdint.h>

#include "cinux/byte_order.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

void ArpCache::init() {
    for (auto& entry : entries_) {
        entry = Entry{};
    }
    next_ = 0;
}

const driver::net::MacAddress* ArpCache::lookup(uint32_t protocol_address) const {
    for (const auto& entry : entries_) {
        if (entry.used && entry.protocol == protocol_address) {
            return &entry.hardware;
        }
    }
    return nullptr;
}

bool ArpCache::observe(const unsigned char* packet, size_t length) {
    const auto kView = ParseArp(packet, length);
    if (!kView.ok()) {
        return false;
    }
    for (auto& entry : entries_) {
        if (entry.used && entry.protocol == kView.value().sender_protocol) {
            entry.hardware = kView.value().sender_hardware;
            return true;
        }
    }
    entries_[next_].used     = true;
    entries_[next_].protocol = kView.value().sender_protocol;
    entries_[next_].hardware = kView.value().sender_hardware;
    next_                    = (next_ + 1) % kArpCacheEntries;
    return true;
}

void ArpCache::build_request(unsigned char* packet, const driver::net::MacAddress& sender_hardware,
                             uint32_t sender_protocol, uint32_t target_protocol) {
    packet[0] = 0x00;
    packet[1] = 0x01;
    packet[2] = 0x08;
    packet[3] = 0x00;
    packet[4] = driver::net::kMacAddressBytes;
    packet[5] = kIpv4AddressBytes;
    packet[6] = 0x00;
    packet[7] = kArpOperRequest;
    for (unsigned long index = 0; index < driver::net::kMacAddressBytes; ++index) {
        packet[8 + index]  = sender_hardware.bytes[index];
        packet[18 + index] = 0;
    }
    base::WriteWord32Be(packet, 14, sender_protocol);
    base::WriteWord32Be(packet, 24, target_protocol);
}

}  // namespace cinux::net
