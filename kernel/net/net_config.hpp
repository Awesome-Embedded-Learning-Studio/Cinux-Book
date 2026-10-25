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
 * @brief   Bytes in one UDP header, fixed shape.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kUdpHeaderBytes = 8;

/**
 * @brief   Listener seats the UDP module keeps, one per bound port.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kUdpPortSeats = 8;

/**
 * @brief   First port the kernel hands out when a caller asks for none.
 * @note    Linux's own ephemeral range, so traces read the same on both
 *          sides of the emulator. Both connection families draw from it.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kEphemeralPortFirst = 32768;

/**
 * @brief   Last port the kernel hands out when a caller asks for none.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr uint16_t kEphemeralPortLast = 60999;

/**
 * @brief   Bytes in one TCP header, fixed shape without options.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpHeaderBytes = 20;

/**
 * @brief   TCP header flag: the sender has no more to say.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned int kTcpFlagFin = 0x01U;

/**
 * @brief   TCP header flag: the sequence numbers synchronize.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned int kTcpFlagSyn = 0x02U;

/**
 * @brief   TCP header flag: the sender asks the connection to die.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned int kTcpFlagRst = 0x04U;

/**
 * @brief   TCP header flag: the sender presses the bytes onward.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned int kTcpFlagPsh = 0x08U;

/**
 * @brief   TCP header flag: the acknowledgement word carries a value.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned int kTcpFlagAck = 0x10U;

/**
 * @brief   Connection seats one TCP module keeps at once.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpConnectionSeats = 8;

/**
 * @brief   Listening ports one TCP module keeps at once.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpListenSeats = 4;

/**
 * @brief   Completed handshakes one listener holds for the taking.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpAcceptQueueSeats = 2;

/**
 * @brief   Received bytes one connection holds for its reader.
 * @note    The window the connection's headers advertise: a full ring
 *          says zero, a drained one opens it again.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpReceiveWindowBytes = 4096;

/**
 * @brief   Sent-but-unacknowledged bytes one connection holds.
 * @note    The send ceiling of a connection that no acknowledgement has
 *          released yet; a write beyond it waits for the peer to catch
 *          up.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpSendWindowBytes = 4096;

/**
 * @brief   First retransmission wait of a connection, in nanoseconds.
 * @note    The classic one-second opener: no round-trip samples exist
 *          when the first byte leaves, so the wait starts conservative
 *          and tightens once samples land.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long long kTcpInitialRtoNanoseconds = 1000ULL * 1000ULL * 1000ULL;

/**
 * @brief   Longest retransmission wait, in nanoseconds.
 * @note    The doubling backs off until here and stays; a dead peer
 *          still costs one deadline per retry, never an infinite spin.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long long kTcpMaxRtoNanoseconds = 64ULL * 1000ULL * 1000ULL * 1000ULL;

/**
 * @brief   Retransmissions one connection spends before giving up.
 * @note    Counted per stretch of silence: an acknowledgement that
 *          moves the send window resets the count and the wait.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpMaxRetransmits = 5;

/**
 * @brief   Duplicate acknowledgements that trip a fast retransmit.
 * @note    Three answers naming the same unacknowledged byte mean the
 *          segment after it was lost, so the oldest unacknowledged
 *          segment leaves at once instead of waiting out the clock.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpFastRetransmitAcks = 3;

/**
 * @brief   Out-of-order segment seats one connection holds for repair.
 * @note    A segment naming sequence numbers ahead of the gap waits
 *          here for the missing bytes; a full table drops the newcomer
 *          by name, never queues unbounded.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpReorderSeats = 4;

/**
 * @brief   One maximum segment lifetime, in nanoseconds.
 * @note    The teaching value: real stacks hold a segment thirty
 *          seconds in the wild, this stack holds one, so a 2MSL wait
 *          fits inside a live demonstration instead of outliving it.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long long kTcpMslNanoseconds = 1000ULL * 1000ULL * 1000ULL;

/**
 * @brief   Clock resolution the round-trip math assumes, in nanoseconds.
 * @note    The classic granularity guard: the computed wait never dips
 *          under it, so a fast local peer still gets a real gap.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long long kTcpRtoGranularityNanoseconds = 100ULL * 1000ULL * 1000ULL;

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
 * @brief   Payload bytes one TCP segment the stack builds may carry.
 * @note    An IPv4 datagram of header, segment, and this payload stays
 *          inside one Ethernet frame, so no honest segment fragments.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kTcpMssBytes =
    kMaxDatagramBytes - kMinIpv4HeaderBytes - kTcpHeaderBytes;

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
 * @brief   Largest echo message the ICMP module mirrors in one reply.
 * @note    The reply rides a single honest IPv4 datagram, so the
 *          mirror ceiling is the datagram budget minus that header;
 *          a request past it is refused by name, never truncated.
 * @since   0.7.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kMaxIcmpEchoBytes = kMaxDatagramBytes - kMinIpv4HeaderBytes;

/**
 * @brief   Nanoseconds one attempt waits before spending itself.
 * @since   0.6.0
 * @ingroup kernel_net
 */
inline constexpr unsigned long kAnswerWaitNanoseconds = 1200UL * 1000UL * 1000UL;

}  // namespace cinux::net
