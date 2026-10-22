#include "../framework/framework.hpp"
#include "kernel/driver/net/loopback.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "test_assert.hpp"

namespace {

const unsigned char kFirst[3]  = {1, 2, 3};
const unsigned char kSecond[2] = {9, 8};

struct FakeNic {
    cinux::driver::net::MacAddress address{};

    [[nodiscard]] bool                           has_mac() const { return true; }
    [[nodiscard]] cinux::driver::net::MacAddress mac() const { return address; }
    [[nodiscard]] bool                           has_ethernet_header() const { return true; }
    [[nodiscard]] unsigned long                  max_frame() const { return 1500; }
    [[nodiscard]] bool poll_frame(cinux::driver::net::RxFrame& /*into*/) { return false; }
    [[nodiscard]] bool send_l3(const cinux::driver::net::MacAddress& /*destination*/,
                               cinux::driver::net::EtherType /*type*/,
                               const unsigned char* /*payload*/, unsigned long /*length*/) {
        return true;
    }
};

}  // namespace

TEST("loopback: a sent payload polls back byte for byte") {
    cinux::driver::net::Loopback::self().init();
    const cinux::driver::net::MacAddress kAny{};
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        kAny, cinux::driver::net::EtherType{.value = 0x0800}, kFirst, sizeof(kFirst)));

    cinux::driver::net::RxFrame frame{};
    ASSERT_TRUE(cinux::driver::net::Loopback::self().poll_frame(frame));
    ASSERT_EQ(frame.size, sizeof(kFirst));
    ASSERT_EQ(frame.bytes[0], 1U);
    ASSERT_EQ(frame.bytes[1], 2U);
    ASSERT_EQ(frame.bytes[2], 3U);
    ASSERT_FALSE(cinux::driver::net::Loopback::self().poll_frame(frame));
}

TEST("loopback: frames keep their send order") {
    cinux::driver::net::Loopback::self().init();
    const cinux::driver::net::MacAddress kAny{};
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        kAny, cinux::driver::net::EtherType{.value = 0x0800}, kFirst, sizeof(kFirst)));
    ASSERT_TRUE(cinux::driver::net::Loopback::self().send_l3(
        kAny, cinux::driver::net::EtherType{.value = 0x0806}, kSecond, sizeof(kSecond)));

    cinux::driver::net::RxFrame frame{};
    ASSERT_TRUE(cinux::driver::net::Loopback::self().poll_frame(frame));
    ASSERT_EQ(frame.size, 3UL);
    ASSERT_TRUE(cinux::driver::net::Loopback::self().poll_frame(frame));
    ASSERT_EQ(frame.size, 2UL);
    ASSERT_EQ(frame.bytes[0], 9U);
}

TEST("loopback: the queue answers full and oversize honestly") {
    cinux::driver::net::Loopback::self().init();
    const cinux::driver::net::MacAddress kAny{};
    const cinux::driver::net::EtherType  kType{.value = 0x0800};
    for (unsigned long index = 0; index < cinux::driver::net::kLoopbackQueueFrames; ++index) {
        ASSERT_TRUE(
            cinux::driver::net::Loopback::self().send_l3(kAny, kType, kFirst, sizeof(kFirst)));
    }
    ASSERT_FALSE(cinux::driver::net::Loopback::self().send_l3(kAny, kType, kFirst, sizeof(kFirst)));

    cinux::driver::net::Loopback::self().init();
    unsigned char oversize[cinux::driver::net::kMaxEthernetFrame + 1] = {};
    ASSERT_FALSE(
        cinux::driver::net::Loopback::self().send_l3(kAny, kType, oversize, sizeof(oversize)));
    ASSERT_FALSE(cinux::driver::net::Loopback::self().send_l3(kAny, kType, kFirst, 0));

    cinux::driver::net::RxFrame frame{};
    ASSERT_FALSE(cinux::driver::net::Loopback::self().poll_frame(frame));
}

TEST("loopback: the honest answers are no address and no header") {
    ASSERT_FALSE(cinux::driver::net::Loopback::self().has_mac());
    ASSERT_FALSE(cinux::driver::net::Loopback::self().has_ethernet_header());
    ASSERT_EQ(cinux::driver::net::Loopback::self().max_frame(),
              cinux::driver::net::kMaxEthernetFrame);
}

TEST("net port: erasure carries the loopback device") {
    cinux::driver::net::Loopback::self().init();

    cinux::driver::net::NetPort loop_port;
    loop_port.adopt(cinux::driver::net::Loopback::self());
    ASSERT_FALSE(loop_port.has_mac());
    ASSERT_FALSE(loop_port.has_ethernet_header());
    ASSERT_EQ(loop_port.max_frame(), cinux::driver::net::kMaxEthernetFrame);
    const cinux::driver::net::MacAddress kAny{};
    ASSERT_TRUE(loop_port.send_l3(kAny, cinux::driver::net::EtherType{.value = 0x0800}, kFirst,
                                  sizeof(kFirst)));
    cinux::driver::net::RxFrame frame{};
    ASSERT_TRUE(loop_port.poll_frame(frame));
    ASSERT_EQ(frame.size, 3UL);
}

TEST("net port: erasure carries a real-shaped device") {
    FakeNic nic{};
    nic.address.bytes[0] = 0x52;
    nic.address.bytes[5] = 0x56;
    cinux::driver::net::NetPort nic_port;
    nic_port.adopt(nic);
    ASSERT_TRUE(nic_port.has_mac());
    ASSERT_EQ(nic_port.mac().bytes[0], 0x52U);
    ASSERT_EQ(nic_port.mac().bytes[5], 0x56U);
    ASSERT_TRUE(nic_port.has_ethernet_header());
    ASSERT_EQ(nic_port.max_frame(), 1500UL);
    cinux::driver::net::RxFrame frame{};
    ASSERT_FALSE(nic_port.poll_frame(frame));
}

TEST("net port: an unadopted port answers nothing") {
    const cinux::driver::net::NetPort    kIdle;
    cinux::driver::net::RxFrame          frame{};
    const cinux::driver::net::MacAddress kAny{};
    ASSERT_FALSE(kIdle.has_mac());
    ASSERT_FALSE(kIdle.has_ethernet_header());
    ASSERT_EQ(kIdle.max_frame(), 0UL);
    ASSERT_FALSE(kIdle.poll_frame(frame));
    ASSERT_FALSE(kIdle.send_l3(kAny, cinux::driver::net::EtherType{.value = 0x0800}, kFirst,
                               sizeof(kFirst)));
}

int main() {
    return cinux::test::RunAll();
}
