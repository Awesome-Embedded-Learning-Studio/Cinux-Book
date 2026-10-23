/**
 * @file    checksum.hpp
 * @brief   The internet one's-complement checksum, RFC 1071.
 *
 * Every internet-layer protocol this kernel speaks — ICMP,
 * IPv4, UDP, TCP — guards its header and payload with the same sum:
 * pair the bytes into big-endian 16-bit words, add them with end-around
 * carry, complement the result. This header is the single home for that
 * sum; protocol code never hand-rolls the fold again. One boundary is
 * deliberate: whether a zero field means "no checksum" is a protocol
 * rule, not arithmetic, and stays with the protocols.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.2
 * @since   0.6.0
 * @ingroup base_bytes
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::base {

/**
 * @brief   Accumulates the RFC 1071 sum of one or more byte spans.
 * @note    Spans may arrive in any split, including across an odd
 *          boundary: a lone trailing byte waits inside and pairs with
 *          the first byte of the next span. Asking for the value of an
 *          odd total treats the dangling byte as the high half of a
 *          zero-padded word, as the RFC prescribes. Feeding a span that
 *          already carries its checksum field must answer zero — that
 *          property is the receiver's whole test.
 * @since   0.6.0
 * @ingroup base_bytes
 */
class InternetChecksum {
public:
    /**
     * @brief         Folds one more byte span into the running sum.
     *
     * @param[in]     data    First byte of the span.
     * @param[in]     length  How many bytes the span holds.
     * @return        None
     * @since         0.6.0
     * @ingroup       base_bytes
     */
    constexpr void add(const unsigned char* data, size_t length) {
        size_t index = 0;
        if (has_pending_ && length != 0) {
            sum_ += pair_word(pending_, data[0]);
            index        = 1;
            has_pending_ = false;
        }
        for (; index + 1 < length; index += 2) {
            sum_ += pair_word(data[index], data[index + 1]);
        }
        if (index < length) {
            pending_     = data[index];
            has_pending_ = true;
        }
    }

    /**
     * @brief         Answers the checksum of everything added so far.
     * @return        The complement of the end-around-carry sum.
     * @note          The accumulator keeps its state: asking twice
     *                without adding between answers the same word, and
     *                more spans may follow a value.
     * @since         0.6.0
     * @ingroup       base_bytes
     */
    [[nodiscard]] constexpr uint16_t value() const {
        uint32_t total = sum_;
        if (has_pending_) {
            total += pair_word(pending_, 0);
        }
        const bit::BitMask<uint32_t> kFolded{fold_carries(total)};
        const bit::BitMask<uint32_t> kChecksumWords{bit::Ones<uint32_t>(16)};
        const bit::BitMask<uint32_t> kChecksum = ~kFolded & kChecksumWords;
        return static_cast<uint16_t>(kChecksum.raw);
    }

private:
    /**
     * @brief         Pairs two bytes into one big-endian 16-bit word.
     *
     * @param[in]     high   The byte the wire sends first.
     * @param[in]     low    The byte the wire sends second.
     * @return        The pair as one unsigned 32-bit word.
     * @since         0.6.0
     * @ingroup       base_bytes
     */
    [[nodiscard]] static constexpr uint32_t pair_word(unsigned char high, unsigned char low) {
        bit::BitMask<uint32_t> word{};
        word.deposit(bit::BitRange{.low = 8, .width = 8}, high);
        word.deposit(bit::BitRange{.low = 0, .width = 8}, low);
        return word.raw;
    }

    /**
     * @brief         Adds the carry bits back until the sum fits in 16.
     *
     * @param[in]     total  The raw sum, carries still sitting above
     *                       bit fifteen.
     * @return        The end-around-carry folded sum.
     * @since         0.6.0
     * @ingroup       base_bytes
     */
    [[nodiscard]] static constexpr uint32_t fold_carries(uint32_t total) {
        const bit::BitMask<uint32_t> kWordBits{bit::Ones<uint32_t>(16)};
        while ((total & kWordBits.raw) != total) {
            const bit::BitMask<uint32_t> kHalves{total};
            total = kHalves.extract(bit::BitRange{.low = 0, .width = 16}) +
                    kHalves.extract(bit::BitRange{.low = 16, .width = 16});
        }
        return total;
    }

    uint32_t      sum_         = 0;
    unsigned char pending_     = 0;
    bool          has_pending_ = false;
};

}  // namespace cinux::base
