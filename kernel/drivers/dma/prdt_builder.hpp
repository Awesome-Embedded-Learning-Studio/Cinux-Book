/**
 * @file kernel/drivers/dma/prdt_builder.hpp
 * @brief PrdtBuilder -- device-independent scatter-gather segment builder
 *
 * A scatter-gather transfer feeds a device a list of physical byte ranges
 * (segments).  Devices differ in table format -- AHCI uses 16-byte PRDT
 * entries (dba/dbau/dbc/i, 22-bit count), NVMe uses PRP pairs, VirtIO chains
 * descriptors -- but they share one need: split a buffer into pieces no larger
 * than a per-segment limit.  PrdtBuilder produces the device-independent list;
 * drivers translate each DmaSegment into their hardware entry.
 *
 * Fixed-capacity (no allocation): the caller picks MaxSegments to match the
 * hardware descriptor table (e.g. AHCI's 8).  add() splits at the device's
 * per-segment limit and stops when the table is full.
 *
 * Namespace: cinux::drivers::dma
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "kernel/drivers/dma/dma_buffer.hpp"

namespace cinux::drivers::dma {

/// Device-independent DMA segment: one contiguous physical range for a single
/// scatter-gather entry.  Drivers convert this to their hardware format.
struct DmaSegment {
    uint64_t phys;  ///< Bus address of the segment start
    uint64_t size;  ///< Length in bytes (> 0)
};

/**
 * @brief Scatter-gather builder with a compile-time segment cap
 *
 * @tparam MaxSegments  Capacity of the segment table (e.g. AHCI = 8).
 *
 * add() / add_buffer() split a range into chunks no larger than the supplied
 * per-segment limit and append them; once the table is full further adds are
 * dropped (returning 0).  Use fits() to check capacity beforehand.
 */
template <std::size_t MaxSegments>
class PrdtBuilder {
public:
    constexpr PrdtBuilder() = default;

    /**
     * @brief Append [phys, phys+size) split into <= @p max_segment chunks
     * @return Segments actually appended (0 if the table is already full, or
     *         if @p max_segment is 0).
     */
    std::size_t add(uint64_t phys, uint64_t size, uint64_t max_segment) {
        if (max_segment == 0) {
            return 0;  // invalid limit -- caller bug; avoids an infinite loop
        }
        std::size_t added    = 0;
        uint64_t    cur      = phys;
        uint64_t    leftover = size;
        while (leftover > 0 && count_ < MaxSegments) {
            uint64_t chunk = leftover < max_segment ? leftover : max_segment;
            segments_[count_++] = {cur, chunk};
            cur += chunk;
            leftover -= chunk;
            ++added;
        }
        return added;
    }

    /**
     * @brief Append a DmaBuffer's whole mapping, split at @p max_segment
     * @return Segments appended (0 if the buffer is invalid or table is full).
     */
    std::size_t add_buffer(const DmaBuffer& buf, uint64_t max_segment) {
        if (!buf.valid()) {
            return 0;
        }
        return add(buf.phys(), buf.size(), max_segment);
    }

    /** @brief Number of segments currently in the table. */
    constexpr std::size_t count() const { return count_; }

    /** @brief Whether the table cannot accept another segment. */
    constexpr bool full() const { return count_ >= MaxSegments; }

    /** @brief Segment @p i (caller must ensure i < count()). */
    const DmaSegment& segment(std::size_t i) const { return segments_[i]; }

    /**
     * @brief Whether @p size bytes at @p max_segment would fit in remaining slots
     *
     * Does not modify the table.  Returns false when @p max_segment is 0 and
     * @p size is non-zero.
     */
    constexpr bool fits(uint64_t size, uint64_t max_segment) const {
        if (max_segment == 0) {
            return size == 0;
        }
        uint64_t needed = (size + max_segment - 1) / max_segment;
        return needed <= (MaxSegments - count_);
    }

private:
    DmaSegment  segments_[MaxSegments]{};
    std::size_t count_ = 0;
};

}  // namespace cinux::drivers::dma
