/**
 * @file    net_layout.hpp
 * @brief   Wire words for the internet family: parse with named refusals.
 *
 * Ethernet, ARP, IPv4, and ICMP echo headers are read through this one
 * word book. Every parse answers either the fields or the exact reason
 * the bytes were refused — a frame too short, a wrong version, a header
 * that disagrees with its own length — because the negative paths are
 * the station's teaching content, not an afterthought. Builders for the
 * sending side join their consumer modules at later batches; this file
 * only reads.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cinux/result.hpp"
#include "kernel/driver/net/net_device.hpp"

namespace cinux::net {

/**
 * @brief   The fields of one Ethernet frame header.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct EthernetView {
    /// Destination hardware address.
    driver::net::MacAddress destination;
    /// Source hardware address.
    driver::net::MacAddress source;
    /// The layer-three protocol the frame carries.
    uint16_t                ethertype = 0;
};

/**
 * @brief   Why an Ethernet frame header was refused.
 * @since   0.6.0
 * @ingroup kernel_net
 */
enum class EthernetError : uint8_t {
    /// The frame ends before the fourteen header bytes do.
    kFrameTooShort,
};

/**
 * @brief         Reads one Ethernet frame header.
 *
 * @param[in]     frame   First byte of the frame.
 * @param[in]     length  Bytes the frame holds.
 * @return        The three header fields, or the refusal.
 * @since         0.6.0
 * @ingroup       kernel_net
 */
base::Result<EthernetView, EthernetError> ParseEthernetHeader(const unsigned char* frame,
                                                              size_t               length);

/**
 * @brief   The fields of one ARP packet over Ethernet and IPv4.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct ArpView {
    /// Sender hardware address.
    driver::net::MacAddress sender_hardware;
    /// Sender protocol address.
    uint32_t                sender_protocol = 0;
    /// Target hardware address.
    driver::net::MacAddress target_hardware;
    /// Target protocol address.
    uint32_t                target_protocol = 0;
    /// Request or reply word.
    uint16_t                operation       = 0;
};

/**
 * @brief   Why an ARP packet was refused.
 * @since   0.6.0
 * @ingroup kernel_net
 */
enum class ArpError : uint8_t {
    /// The packet ends before the fixed shape does.
    kPacketTooShort,
    /// The hardware space is not Ethernet.
    kUnexpectedHardware,
    /// The protocol space is not IPv4.
    kUnexpectedProtocol,
    /// The address sizes are not six and four bytes.
    kUnexpectedAddressSizes,
};

/**
 * @brief         Reads one ARP packet.
 *
 * @param[in]     packet  First byte of the packet.
 * @param[in]     length  Bytes the packet holds.
 * @return        The resolved addresses, or the refusal.
 * @note          This family speaks exactly one shape: ARP over
 *                Ethernet carrying IPv4. Everything else is refused
 *                by name instead of parsed loosely.
 * @since         0.6.0
 * @ingroup       kernel_net
 */
base::Result<ArpView, ArpError> ParseArp(const unsigned char* packet, size_t length);

/**
 * @brief   The fields of one IPv4 header the stack needs.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct Ipv4View {
    /// Header size in bytes, always twenty through sixty.
    unsigned char header_bytes = 0;
    /// Whole-datagram length the header announces.
    uint16_t      total_length = 0;
    /// Layer-four protocol number.
    unsigned char protocol     = 0;
    /// Source address.
    uint32_t      source       = 0;
    /// Destination address.
    uint32_t      destination  = 0;
};

/**
 * @brief   Why an IPv4 header was refused.
 * @since   0.6.0
 * @ingroup kernel_net
 */
enum class Ipv4Error : uint8_t {
    /// The datagram ends before the smallest header does.
    kDatagramTooShort,
    /// The version nibble is not four.
    kBadVersion,
    /// The header-length nibble is below five words.
    kBadHeaderWords,
    /// The announced total is shorter than the header itself.
    kBadTotalLength,
};

/**
 * @brief         Reads one IPv4 header.
 *
 * @param[in]     packet  First byte of the datagram.
 * @param[in]     length  Bytes the datagram holds.
 * @return        The header fields, or the refusal.
 * @note          The parse judges the header against itself; whether
 *                the announced total fits the buffer it arrived in is
 *                the receiving module's question, not the word book's.
 * @since         0.6.0
 * @ingroup       kernel_net
 */
base::Result<Ipv4View, Ipv4Error> ParseIpv4(const unsigned char* packet, size_t length);

/**
 * @brief   The fields of one ICMP echo header, request or reply.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct IcmpEchoView {
    /// Echo request or echo reply word.
    unsigned char type       = 0;
    /// Always zero for echo traffic.
    unsigned char code       = 0;
    /// The checksum as it arrived, for the receiver to verify.
    uint16_t      checksum   = 0;
    /// Caller-chosen pairing word.
    uint16_t      identifier = 0;
    /// Caller-chosen ordering word.
    uint16_t      sequence   = 0;
};

/**
 * @brief   Why an ICMP echo header was refused.
 * @since   0.6.0
 * @ingroup kernel_net
 */
enum class IcmpEchoError : uint8_t {
    /// The datagram ends before the eight header bytes do.
    kDatagramTooShort,
};

/**
 * @brief         Reads one ICMP echo header.
 *
 * @param[in]     datagram  First byte of the datagram.
 * @param[in]     length    Bytes the datagram holds.
 * @return        The echo fields, or the refusal.
 * @since         0.6.0
 * @ingroup       kernel_net
 */
base::Result<IcmpEchoView, IcmpEchoError> ParseIcmpEcho(const unsigned char* datagram,
                                                        size_t               length);

}  // namespace cinux::net
