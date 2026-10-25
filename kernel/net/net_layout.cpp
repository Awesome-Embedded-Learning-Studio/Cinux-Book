#include "kernel/net/net_layout.hpp"

#include <stddef.h>
#include <stdint.h>

#include "cinux/bit_ops/bit_ops.hpp"
#include "cinux/byte_order.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/net/net_config.hpp"
#include "kernel/driver/net/net_device.hpp"
#include "kernel/net/net_config.hpp"

namespace cinux::net {

namespace {

void copy_mac(driver::net::MacAddress& into, const unsigned char* from) {
    for (unsigned long index = 0; index < driver::net::kMacAddressBytes; ++index) {
        into.bytes[index] = from[index];
    }
}

}  // namespace

base::Result<EthernetView, EthernetError> ParseEthernetHeader(const unsigned char* frame,
                                                              size_t               length) {
    if (length < kEthernetHeaderBytes) {
        return EthernetError::kFrameTooShort;
    }
    EthernetView view{};
    copy_mac(view.destination, frame);
    copy_mac(view.source, frame + driver::net::kMacAddressBytes);
    view.ethertype = base::ReadWord16Be(frame, 2 * driver::net::kMacAddressBytes);
    return view;
}

base::Result<ArpView, ArpError> ParseArp(const unsigned char* packet, size_t length) {
    if (length < kArpPacketBytes) {
        return ArpError::kPacketTooShort;
    }
    if (base::ReadWord16Be(packet, 0) != kArpHardwareEthernet) {
        return ArpError::kUnexpectedHardware;
    }
    if (base::ReadWord16Be(packet, 2) != kEtherTypeIpv4) {
        return ArpError::kUnexpectedProtocol;
    }
    if (packet[4] != driver::net::kMacAddressBytes || packet[5] != kIpv4AddressBytes) {
        return ArpError::kUnexpectedAddressSizes;
    }
    ArpView view{};
    copy_mac(view.sender_hardware, packet + 8);
    view.sender_protocol = base::ReadWord32Be(packet, 14);
    copy_mac(view.target_hardware, packet + 18);
    view.target_protocol = base::ReadWord32Be(packet, 24);
    view.operation       = base::ReadWord16Be(packet, 6);
    return view;
}

base::Result<Ipv4View, Ipv4Error> ParseIpv4(const unsigned char* packet, size_t length) {
    if (length < kMinIpv4HeaderBytes) {
        return Ipv4Error::kDatagramTooShort;
    }
    if (base::bit::HighNibble(packet[0]) != kIpv4Version) {
        return Ipv4Error::kBadVersion;
    }
    const unsigned char kHeaderWords = base::bit::LowNibble(packet[0]);
    if (kHeaderWords < kMinIpv4HeaderBytes / sizeof(uint32_t)) {
        return Ipv4Error::kBadHeaderWords;
    }
    const auto kHeaderBytes = static_cast<unsigned char>(kHeaderWords * sizeof(uint32_t));
    if (length < kHeaderBytes) {
        return Ipv4Error::kDatagramTooShort;
    }
    const uint16_t kTotalLength = base::ReadWord16Be(packet, 2);
    if (kTotalLength < kHeaderBytes) {
        return Ipv4Error::kBadTotalLength;
    }
    Ipv4View view{};
    view.header_bytes = kHeaderBytes;
    view.total_length = kTotalLength;
    view.protocol     = packet[9];
    view.source       = base::ReadWord32Be(packet, 12);
    view.destination  = base::ReadWord32Be(packet, 16);
    return view;
}

base::Result<IcmpEchoView, IcmpEchoError> ParseIcmpEcho(const unsigned char* datagram,
                                                        size_t               length) {
    if (length < kIcmpEchoHeaderBytes) {
        return IcmpEchoError::kDatagramTooShort;
    }
    IcmpEchoView view{};
    view.type       = datagram[0];
    view.code       = datagram[1];
    view.checksum   = base::ReadWord16Be(datagram, 2);
    view.identifier = base::ReadWord16Be(datagram, 4);
    view.sequence   = base::ReadWord16Be(datagram, 6);
    return view;
}

base::Result<UdpView, UdpError> ParseUdp(const unsigned char* datagram, size_t length) {
    if (length < kUdpHeaderBytes) {
        return UdpError::kDatagramTooShort;
    }
    UdpView view{};
    view.source_port      = base::ReadWord16Be(datagram, 0);
    view.destination_port = base::ReadWord16Be(datagram, 2);
    view.length           = base::ReadWord16Be(datagram, 4);
    view.checksum         = base::ReadWord16Be(datagram, 6);
    return view;
}

base::Result<TcpView, TcpError> ParseTcp(const unsigned char* segment, size_t length) {
    if (length < kTcpHeaderBytes) {
        return TcpError::kSegmentTooShort;
    }
    const unsigned char kHeaderWords = base::bit::HighNibble(segment[12]);
    if (kHeaderWords < kTcpHeaderBytes / sizeof(uint32_t)) {
        return TcpError::kBadDataOffset;
    }
    const auto kHeaderBytes = static_cast<unsigned char>(kHeaderWords * sizeof(uint32_t));
    if (kHeaderBytes > length) {
        return TcpError::kHeaderPastBytes;
    }
    TcpView view{};
    view.source_port      = base::ReadWord16Be(segment, 0);
    view.destination_port = base::ReadWord16Be(segment, 2);
    view.sequence         = base::ReadWord32Be(segment, 4);
    view.acknowledgement  = base::ReadWord32Be(segment, 8);
    view.header_bytes     = kHeaderBytes;
    view.flags            = segment[13];
    view.window           = base::ReadWord16Be(segment, 14);
    view.checksum         = base::ReadWord16Be(segment, 16);
    return view;
}

}  // namespace cinux::net
