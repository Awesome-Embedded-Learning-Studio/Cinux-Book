#include <stdint.h>

#include "../framework/framework.hpp"
#include "cinux/byte_order.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "test_assert.hpp"

namespace {

const unsigned char kEthernetFrame[cinux::net::kEthernetHeaderBytes] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x08, 0x00,
};

const unsigned char kArpReply[cinux::net::kArpPacketBytes] = {
    0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x0a, 0x00, 0x02, 0x0f, 0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 0x0a, 0x00, 0x02, 0x02,
};

void copy_arp_reply(unsigned char* packet) {
    for (unsigned long index = 0; index < cinux::net::kArpPacketBytes; ++index) {
        packet[index] = kArpReply[index];
    }
}

void fill_ipv4_header(unsigned char* packet) {
    packet[0]  = 0x45;
    packet[1]  = 0x00;
    packet[2]  = 0x00;
    packet[3]  = 0x14;
    packet[9]  = cinux::net::kIpv4ProtocolIcmp;
    packet[12] = 0x0a;
    packet[13] = 0x00;
    packet[14] = 0x02;
    packet[15] = 0x0f;
    packet[16] = 0x0a;
    packet[17] = 0x00;
    packet[18] = 0x02;
    packet[19] = 0x02;
}

void copy_ipv4_header(unsigned char* packet) {
    unsigned char source[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(source);
    for (unsigned long index = 0; index < cinux::net::kMinIpv4HeaderBytes; ++index) {
        packet[index] = source[index];
    }
}

}  // namespace

TEST("ethernet: a full frame parses into addresses and type") {
    const auto kView = cinux::net::ParseEthernetHeader(kEthernetFrame, sizeof(kEthernetFrame));
    ASSERT_TRUE(kView.ok());
    ASSERT_TRUE(kView.value().destination.bytes[0] == 0x52);
    ASSERT_TRUE(kView.value().destination.bytes[5] == 0x56);
    ASSERT_TRUE(kView.value().source.bytes[0] == 0xaa);
    ASSERT_TRUE(kView.value().source.bytes[5] == 0xff);
    ASSERT_TRUE(kView.value().ethertype == cinux::net::kEtherTypeIpv4);
}

TEST("ethernet: a frame short of fourteen bytes is refused by name") {
    const auto kView =
        cinux::net::ParseEthernetHeader(kEthernetFrame, cinux::net::kEthernetHeaderBytes - 1);
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::EthernetError::kFrameTooShort);
}

TEST("arp: an ethernet reply resolves both address pairs") {
    const auto kView = cinux::net::ParseArp(kArpReply, sizeof(kArpReply));
    ASSERT_TRUE(kView.ok());
    ASSERT_TRUE(kView.value().operation == cinux::net::kArpOperReply);
    ASSERT_TRUE(kView.value().sender_hardware.bytes[0] == 0xaa);
    ASSERT_TRUE(kView.value().sender_protocol == 0x0a00020f);
    ASSERT_TRUE(kView.value().target_hardware.bytes[5] == 0x56);
    ASSERT_TRUE(kView.value().target_protocol == 0x0a000202);
}

TEST("arp: a truncated packet is refused as too short") {
    const auto kView = cinux::net::ParseArp(kArpReply, cinux::net::kArpPacketBytes - 1);
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::ArpError::kPacketTooShort);
}

TEST("arp: a foreign hardware space is refused by name") {
    unsigned char packet[cinux::net::kArpPacketBytes];
    copy_arp_reply(packet);
    packet[1]        = 0x06;
    const auto kView = cinux::net::ParseArp(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::ArpError::kUnexpectedHardware);
}

TEST("arp: a non-IPv4 protocol space is refused by name") {
    unsigned char packet[cinux::net::kArpPacketBytes];
    copy_arp_reply(packet);
    packet[3]        = 0xdd;
    const auto kView = cinux::net::ParseArp(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::ArpError::kUnexpectedProtocol);
}

TEST("arp: wrong address sizes are refused by name") {
    unsigned char packet[cinux::net::kArpPacketBytes];
    copy_arp_reply(packet);
    packet[4]        = 0x08;
    const auto kView = cinux::net::ParseArp(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::ArpError::kUnexpectedAddressSizes);
}

TEST("ipv4: a minimal header reads its fields") {
    unsigned char packet[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(packet);
    const auto kView = cinux::net::ParseIpv4(packet, sizeof(packet));
    ASSERT_TRUE(kView.ok());
    ASSERT_TRUE(kView.value().header_bytes == 20);
    ASSERT_TRUE(kView.value().total_length == 20);
    ASSERT_TRUE(kView.value().protocol == cinux::net::kIpv4ProtocolIcmp);
    ASSERT_TRUE(kView.value().source == 0x0a00020f);
    ASSERT_TRUE(kView.value().destination == 0x0a000202);
}

TEST("ipv4: a datagram short of twenty bytes is refused as too short") {
    unsigned char packet[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(packet);
    const auto kView = cinux::net::ParseIpv4(packet, cinux::net::kMinIpv4HeaderBytes - 1);
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::Ipv4Error::kDatagramTooShort);
}

TEST("ipv4: a header longer than the bytes that arrived is refused") {
    unsigned char packet[cinux::net::kMinIpv4HeaderBytes] = {};
    fill_ipv4_header(packet);
    packet[0]        = 0x4F;
    const auto kView = cinux::net::ParseIpv4(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::Ipv4Error::kDatagramTooShort);
}

TEST("ipv4: a version other than four is refused by name") {
    unsigned char packet[cinux::net::kMinIpv4HeaderBytes] = {};
    copy_ipv4_header(packet);
    packet[0]        = 0x55;
    const auto kView = cinux::net::ParseIpv4(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::Ipv4Error::kBadVersion);
}

TEST("ipv4: a header under five words is refused by name") {
    unsigned char packet[cinux::net::kMinIpv4HeaderBytes] = {};
    copy_ipv4_header(packet);
    packet[0]        = 0x44;
    const auto kView = cinux::net::ParseIpv4(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::Ipv4Error::kBadHeaderWords);
}

TEST("ipv4: a total shorter than its own header is refused by name") {
    unsigned char packet[24] = {};
    copy_ipv4_header(packet);
    packet[0]        = 0x46;
    const auto kView = cinux::net::ParseIpv4(packet, sizeof(packet));
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::Ipv4Error::kBadTotalLength);
}

TEST("icmp: an echo header reads type, checksum, and pairing words") {
    unsigned char packet[cinux::net::kIcmpEchoHeaderBytes] = {};
    packet[0]                                              = cinux::net::kIcmpEchoRequest;
    packet[1]                                              = 0x00;
    packet[2]                                              = 0xab;
    packet[3]                                              = 0xcd;
    packet[4]                                              = 0x12;
    packet[5]                                              = 0x34;
    packet[6]                                              = 0x00;
    packet[7]                                              = 0x01;
    const auto kView = cinux::net::ParseIcmpEcho(packet, sizeof(packet));
    ASSERT_TRUE(kView.ok());
    ASSERT_TRUE(kView.value().type == cinux::net::kIcmpEchoRequest);
    ASSERT_TRUE(kView.value().code == 0);
    ASSERT_TRUE(kView.value().checksum == 0xabcd);
    ASSERT_TRUE(kView.value().identifier == 0x1234);
    ASSERT_TRUE(kView.value().sequence == 0x0001);
}

TEST("icmp: a datagram short of eight bytes is refused as too short") {
    unsigned char packet[cinux::net::kIcmpEchoHeaderBytes] = {};
    packet[0]                                              = cinux::net::kIcmpEchoRequest;
    const auto kView = cinux::net::ParseIcmpEcho(packet, cinux::net::kIcmpEchoHeaderBytes - 1);
    ASSERT_TRUE(!kView.ok());
    ASSERT_TRUE(kView.error() == cinux::net::IcmpEchoError::kDatagramTooShort);
}

TEST("byte order: big-endian reads spell the wire order") {
    const unsigned char kWords[6] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    };
    ASSERT_TRUE(cinux::base::ReadWord16Be(kWords, 0) == 0x1234);
    ASSERT_TRUE(cinux::base::ReadWord32Be(kWords, 2) == 0x56789abc);
}

int main() {
    return cinux::test::RunAll();
}
