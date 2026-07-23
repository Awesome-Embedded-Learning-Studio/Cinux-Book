/**
 * @file test/unit/test_net_arp_cache.cpp
 * @brief Host unit tests for ArpCache (insert / lookup / miss / update /
 *        round-robin eviction / clear).
 *
 * Header-only type (kernel/net/arp.hpp) -- no kernel sources linked.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include "kernel/net/arp.hpp"
#include "test_framework.h"

using cinux::net::ArpCache;
using cinux::net::EthAddr;
using cinux::net::Ipv4Addr;

namespace {

Ipv4Addr ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return Ipv4Addr{{a, b, c, d}};
}
EthAddr mac(uint8_t v) {
    EthAddr e{};
    for (int i = 0; i < 6; ++i) {
        e.oct[i] = v;
    }
    return e;
}

}  // namespace

TEST("arp_cache: empty cache misses") {
    ArpCache c;
    EthAddr  out{};
    ASSERT_FALSE(c.lookup(ip(10, 0, 2, 2), out));
}

TEST("arp_cache: insert then hit") {
    ArpCache c;
    c.insert(ip(10, 0, 2, 2), mac(0xAA));
    EthAddr out{};
    ASSERT_TRUE(c.lookup(ip(10, 0, 2, 2), out));
    ASSERT_EQ(out.oct[0], 0xAA);
}

TEST("arp_cache: miss on a different IP") {
    ArpCache c;
    c.insert(ip(10, 0, 2, 2), mac(0xAA));
    EthAddr out{};
    ASSERT_FALSE(c.lookup(ip(10, 0, 2, 3), out));
}

TEST("arp_cache: insert refreshes an existing entry") {
    ArpCache c;
    c.insert(ip(10, 0, 2, 2), mac(0xAA));
    c.insert(ip(10, 0, 2, 2), mac(0xBB));  // same IP, new MAC
    EthAddr out{};
    ASSERT_TRUE(c.lookup(ip(10, 0, 2, 2), out));
    ASSERT_EQ(out.oct[0], 0xBB);  // updated, not duplicated
}

TEST("arp_cache: round-robin eviction after kSlots distinct IPs") {
    ArpCache c;
    for (uint8_t i = 0; i < ArpCache::kSlots; ++i) {
        c.insert(ip(10, 0, 0, i), mac(i));  // fill all slots
    }
    // All kSlots still present.
    EthAddr out{};
    for (uint8_t i = 0; i < ArpCache::kSlots; ++i) {
        ASSERT_TRUE(c.lookup(ip(10, 0, 0, i), out));
    }
    // kSlots+1-th distinct IP evicts the slot at the round-robin cursor (index 0).
    c.insert(ip(10, 0, 1, 1), mac(0x99));
    ASSERT_FALSE(c.lookup(ip(10, 0, 0, 0), out));  // first inserted evicted
    ASSERT_TRUE(c.lookup(ip(10, 0, 1, 1), out));   // new one present
    ASSERT_TRUE(c.lookup(ip(10, 0, 0, 1), out));   // second inserted still alive
}

TEST("arp_cache: clear empties the cache") {
    ArpCache c;
    c.insert(ip(10, 0, 2, 2), mac(0xAA));
    c.insert(ip(10, 0, 2, 3), mac(0xBB));
    c.clear();
    EthAddr out{};
    ASSERT_FALSE(c.lookup(ip(10, 0, 2, 2), out));
    ASSERT_FALSE(c.lookup(ip(10, 0, 2, 3), out));
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
