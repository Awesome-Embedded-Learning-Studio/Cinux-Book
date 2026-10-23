#include <stdint.h>

#include "../framework/framework.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/arp.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "test_assert.hpp"

namespace {

const unsigned char kArpReply[cinux::net::kArpPacketBytes] = {
    0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x0a, 0x00, 0x02, 0x0f, 0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 0x0a, 0x00, 0x02, 0x02,
};

}  // namespace

TEST("arp: a built request parses back as a request") {
    unsigned char                        packet[cinux::net::kArpPacketBytes] = {};
    const cinux::driver::net::MacAddress kSelf{
        {
            0x52,
            0x54,
            0x00,
            0x12,
            0x34,
            0x56,
        },
    };
    cinux::net::ArpCache::build_request(packet, kSelf, cinux::net::kLoopbackAddress, 0x0a000202);
    const auto kView = cinux::net::ParseArp(packet, sizeof(packet));
    ASSERT_TRUE(kView.ok());
    ASSERT_TRUE(kView.value().operation == cinux::net::kArpOperRequest);
    ASSERT_TRUE(kView.value().sender_hardware.bytes[0] == 0x52);
    ASSERT_TRUE(kView.value().sender_protocol == cinux::net::kLoopbackAddress);
    ASSERT_TRUE(kView.value().target_hardware.bytes[0] == 0x00);
    ASSERT_TRUE(kView.value().target_protocol == 0x0a000202);
}

TEST("arp: a reply teaches the sender pair only") {
    cinux::net::ArpCache cache;
    cache.init();
    ASSERT_TRUE(cache.observe(kArpReply, sizeof(kArpReply)));
    const cinux::driver::net::MacAddress* learned = cache.lookup(0x0a00020f);
    ASSERT_TRUE(learned != nullptr);
    ASSERT_TRUE(learned->bytes[0] == 0xaa);
    ASSERT_TRUE(learned->bytes[5] == 0xff);
    ASSERT_TRUE(cache.lookup(0x0a000202) == nullptr);
}

TEST("arp: a refused packet teaches nothing") {
    cinux::net::ArpCache cache;
    cache.init();
    ASSERT_TRUE(!cache.observe(kArpReply, cinux::net::kArpPacketBytes - 1));
    ASSERT_TRUE(cache.lookup(0x0a00020f) == nullptr);
}

TEST("arp: a request teaches its asker, not its target") {
    unsigned char                        packet[cinux::net::kArpPacketBytes] = {};
    const cinux::driver::net::MacAddress kSelf{
        {
            0x52,
            0x54,
            0x00,
            0x12,
            0x34,
            0x56,
        },
    };
    cinux::net::ArpCache::build_request(packet, kSelf, cinux::net::kLoopbackAddress, 0x0a000202);
    cinux::net::ArpCache cache;
    cache.init();
    ASSERT_TRUE(cache.observe(packet, sizeof(packet)));
    ASSERT_TRUE(cache.lookup(cinux::net::kLoopbackAddress) != nullptr);
    ASSERT_TRUE(cache.lookup(0x0a000202) == nullptr);
}

TEST("arp: a later answer replaces the learned hardware address") {
    cinux::net::ArpCache cache;
    cache.init();
    ASSERT_TRUE(cache.observe(kArpReply, sizeof(kArpReply)));
    unsigned char updated[cinux::net::kArpPacketBytes];
    for (unsigned long index = 0; index < cinux::net::kArpPacketBytes; ++index) {
        updated[index] = kArpReply[index];
    }
    updated[8]  = 0x11;
    updated[13] = 0x22;
    ASSERT_TRUE(cache.observe(updated, sizeof(updated)));
    const cinux::driver::net::MacAddress* learned = cache.lookup(0x0a00020f);
    ASSERT_TRUE(learned != nullptr);
    ASSERT_TRUE(learned->bytes[0] == 0x11);
    ASSERT_TRUE(learned->bytes[5] == 0x22);
}

int main() {
    return cinux::test::RunAll();
}
