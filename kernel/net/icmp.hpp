/**
 * @file    icmp.hpp
 * @brief   The ICMP module: answer echoes, count replies.
 *
 * Echo requests are answered on the spot — same identifier, same
 * sequence, payload copied, checksum recomputed over the zeroed field —
 * and aimed back at the asker through the sink the wiring points at the
 * IPv4 module. Echo replies are not handled; they are observed, and the
 * counters are the ping demo's whole scoreboard. Everything that fails
 * the word book or the checksum folds into its own counter, silently:
 * the discipline of this family is that bad packets die counted deaths.
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

#include "kernel/net/ipv4.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"

namespace cinux::net {

/**
 * @brief   The echo scoreboard and its refused corner.
 * @since   0.6.0
 * @ingroup kernel_net
 */
struct IcmpStats {
    /// Echo requests answered.
    unsigned long requests_answered = 0;
    /// Echo replies observed.
    unsigned long replies_seen      = 0;
    /// Identifier of the last observed reply.
    uint16_t      last_identifier   = 0;
    /// Sequence of the last observed reply.
    uint16_t      last_sequence     = 0;
    /// Messages the word book refused as too short.
    unsigned long refused_short     = 0;
    /// Messages whose checksum did not fold to zero.
    unsigned long refused_checksum  = 0;
};

/**
 * @brief   The ICMP module, a plain instance the wiring owns.
 * @since   0.6.0
 * @ingroup kernel_net
 */
class IcmpModule {
public:
    /**
     * @brief         Aims the module at its reply path.
     *
     * @param[in]     reply_sink  Where echo replies go; the wiring
     *                            points this at the IPv4 module.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void init(const DatagramSink& reply_sink);

    /**
     * @brief         Judges one ICMP message, answers or observes it.
     *
     * @param[in]     bytes   First message byte.
     * @param[in]     length  Bytes the message holds.
     * @param[in]     header  The IPv4 header it travelled in.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    void handle(const unsigned char* bytes, size_t length, const Ipv4View& header);

    /**
     * @brief         The scoreboard so far.
     * @return        A reference to the current statistics.
     * @since         0.6.0
     * @ingroup       kernel_net
     */
    [[nodiscard]] const IcmpStats& stats() const;

private:
    DatagramSink  reply_sink_                                     = {};
    IcmpStats     stats_                                          = {};
    unsigned char reply_[kMaxDatagramBytes - kMinIpv4HeaderBytes] = {};
};

/**
 * @brief         Writes one echo request into a caller-owned buffer.
 *
 * @param[out]    message         Buffer of header plus payload bytes.
 * @param[in]     length          Whole message length, payload included.
 * @param[in]     identifier      Pairing word for this conversation.
 * @param[in]     sequence        Ordering word for this message.
 * @return        None
 * @note          The payload bytes are filled with an index pattern and
 *                the checksum is computed over the zeroed field, so the
 *                receiver's fold-to-zero property holds as sent.
 * @since         0.6.0
 * @ingroup       kernel_net
 */
void BuildEchoRequest(unsigned char* message, size_t length, uint16_t identifier,
                      uint16_t sequence);

}  // namespace cinux::net
