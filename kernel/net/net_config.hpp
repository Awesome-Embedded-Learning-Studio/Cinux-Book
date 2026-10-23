/**
 * @file    net_config.hpp
 * @brief   Capacity and shape words for the internet protocol family.
 *
 * The wire spells every field big-endian and fixes most sizes, so the
 * family keeps its own fact book here: header lengths, ethertype and
 * protocol numbers, and the fixed ARP-over-Ethernet packet shape. The
 * device-side words (MAC length, frame ceiling) live with the device
 * family in kernel/driver/net and are reused, never restated.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "kernel/driver/net/net_config.hpp"

namespace cinux::net {

using driver::net::kEthernetHeaderBytes;

/**
 * @brief   Ethertype word naming an IPv4 payload.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kEtherTypeIpv4 = 0x0800;

/**
 * @brief   Ethertype word naming an ARP payload.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kEtherTypeArp = 0x0806;

/**
 * @brief   Bytes in one ARP packet over Ethernet and IPv4, fixed shape.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kArpPacketBytes = 28;

/**
 * @brief   ARP hardware-space word for Ethernet.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kArpHardwareEthernet = 1;

/**
 * @brief   ARP operation word for a request.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kArpOperRequest = 1;

/**
 * @brief   ARP operation word for a reply.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kArpOperReply = 2;

/**
 * @brief   The only IPv4 version this stack speaks.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIpv4Version = 4;

/**
 * @brief   Bytes in one IPv4 address.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kIpv4AddressBytes = 4;

/**
 * @brief   Smallest legal IPv4 header, five 32-bit words.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMinIpv4HeaderBytes = 20;

/**
 * @brief   Largest legal IPv4 header, fifteen 32-bit words.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxIpv4HeaderBytes = 60;

/**
 * @brief   IPv4 protocol number naming ICMP.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIpv4ProtocolIcmp = 1;

/**
 * @brief   IPv4 protocol number naming TCP.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIpv4ProtocolTcp = 6;

/**
 * @brief   IPv4 protocol number naming UDP.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIpv4ProtocolUdp = 17;

/**
 * @brief   Bytes in one ICMP echo header, request or reply.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kIcmpEchoHeaderBytes = 8;

/**
 * @brief   ICMP type word for an echo request.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIcmpEchoRequest = 8;

/**
 * @brief   ICMP type word for an echo reply.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIcmpEchoReply = 0;

/**
 * @brief   Device ports one protocol stack serves at once.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxPorts = 2;

/**
 * @brief   Ethertype seats one stack keeps.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxProtocolSeats = 4;

/**
 * @brief   Layer-four seats one IPv4 module keeps.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxL4Seats = 4;

/**
 * @brief   Frames one poll call may deliver, the re-entry guard.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kPollBudgetFrames = 16;

/**
 * @brief   Largest datagram the stack builds, header plus payload.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxDatagramBytes = 1500;

/**
 * @brief   The loopback address, 127.0.0.1.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint32_t kLoopbackAddress = 0x7f000001;

/**
 * @brief   The loopback network's mask: the top byte alone.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint32_t kLoopbackNetMask = 0xFF000000;

/**
 * @brief   The loopback network itself, 127.0.0.0.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint32_t kLoopbackNet = 0x7F000000;

/**
 * @brief   The kernel's address on the QEMU user network, 10.0.2.15.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint32_t kUserNetworkSelf = 0x0a00020f;

/**
 * @brief   The user network gateway and echo answerer, 10.0.2.2.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint32_t kUserNetworkHost = 0x0a000202;

/**
 * @brief   Time-to-live every outgoing IPv4 header carries.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned char kIpv4TimeToLive = 64;

/**
 * @brief   Address pairs the ARP cache keeps.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kArpCacheEntries = 4;

/**
 * @brief   Bytes in one demo echo message, header plus a small payload.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kEchoMessageBytes = 12;

/**
 * @brief   Nanoseconds one attempt waits before spending itself.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kAnswerWaitNanoseconds = 1200UL * 1000UL * 1000UL;

}  // namespace cinux::net
