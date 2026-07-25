/**
 * @file kernel/drivers/virtio/virtqueue.hpp
 * @brief VirtQueue -- VirtIO split virtqueue (F5-M2 batch 1+2)
 *
 * Three DMA-coherent tables (desc / avail / used) shared between driver and
 * device.  Completion polled in batch 1-2 (wait_completion spins until used->idx
 * catches up); batch 3 replaces the spin with an MSI-X interrupt wake.
 *
 * Descriptor allocation.  submit_one / submit_chain use desc[0..n-1] from a
 * FIXED start index, NOT a free-list.  The caller (VirtIOBlock / VirtIONet)
 * serialises submit -> kick -> wait_completion under a per-device Spinlock, so
 * a previous chain is always consumed before the next submits.  This mirrors
 * NvmeBlockDevice's single-DmaBuffer + Spinlock pattern (the NVMe SMP race
 * 749e7db diagnosed); a free-list + recycling is a follow-up if concurrency
 * ever widens past one in-flight request per device.
 *
 * Namespace: cinux::drivers::virtio
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/virtio/virtio.hpp"

namespace cinux::drivers::virtio {

// ============================================================
// Descriptor flags (VqDesc.flags)
// ============================================================
namespace DescFlag {
constexpr uint16_t NEXT  = 0x01;  ///< this desc chains to desc[next]
constexpr uint16_t WRITE = 0x02;  ///< device-written (device -> driver direction)
}  // namespace DescFlag

// ============================================================
// Ring flags (avail/used flags)
// ============================================================
namespace RingFlag {
constexpr uint16_t NO_INTERRUPT = 0x01;  ///< avail: suppress notify-on-consume
}  // namespace RingFlag

#pragma pack(push, 1)

/// Descriptor table entry (16 bytes).
struct VqDesc {
    uint64_t addr;    ///< DMA address of the buffer
    uint32_t len;     ///< length in bytes
    uint16_t flags;
    uint16_t next;    ///< next desc index (valid iff flags & NEXT)
};
static_assert(sizeof(VqDesc) == 16, "VqDesc must be 16 bytes");

/// Used ring completion entry (8 bytes).
struct VqUsedElem {
    uint32_t id;    ///< head desc index of the completed chain
    uint32_t len;   ///< total bytes written (for WRITE descs)
};
static_assert(sizeof(VqUsedElem) == 8, "VqUsedElem must be 8 bytes");

#pragma pack(pop)

/// Scatter-gather entry for submit_chain(): one DMA buffer segment.
/// @p phys is the bus address, @p len the byte count.
struct SgEntry {
    uint64_t phys;
    uint32_t len;
};

// avail/used ring header offsets (dynamic-size rings, accessed by offset).
namespace AvailOff {
constexpr uint32_t FLAGS = 0;  ///< uint16
constexpr uint32_t IDX   = 2;  ///< uint16 (free-running, driver increments)
constexpr uint32_t RING  = 4;  ///< uint16[size] -- indices into desc table
}  // namespace AvailOff

namespace UsedOff {
constexpr uint32_t FLAGS = 0;  ///< uint16
constexpr uint32_t IDX   = 2;  ///< uint16 (free-running, device increments)
constexpr uint32_t RING  = 4;  ///< VqUsedElem[size]
}  // namespace UsedOff

/// DMA byte sizes for the three tables of a @p qsize-entry queue.
constexpr uint64_t desc_table_bytes(uint16_t qsize) {
    return static_cast<uint64_t>(qsize) * sizeof(VqDesc);
}
constexpr uint64_t avail_ring_bytes(uint16_t qsize) {
    return 4 + 2 * static_cast<uint64_t>(qsize) + 2;
}
constexpr uint64_t used_ring_bytes(uint16_t qsize) {
    return 4 + sizeof(VqUsedElem) * static_cast<uint64_t>(qsize) + 2;
}

/**
 * @brief A VirtIO split virtqueue backed by three DMA buffers
 */
class VirtQueue {
public:
    VirtQueue() = default;
    ~VirtQueue() = default;

    VirtQueue(const VirtQueue&)            = delete;
    VirtQueue& operator=(const VirtQueue&) = delete;
    VirtQueue(VirtQueue&&) noexcept            = default;
    VirtQueue& operator=(VirtQueue&&) noexcept = default;

    /// Allocate the three tables, zero them, register + enable the queue via
    /// @p dev.  Stores the notify_off returned by setup_queue for kick().
    cinux::lib::ErrorOr<void> init(VirtIODevice* dev, uint16_t queue_index, uint16_t qsize);

    /// Publish a single-buffer request at DMA @p phys (desc[0], fixed).  Serial
    /// caller guarantees no overlap with a previous waited submission.
    cinux::lib::ErrorOr<uint16_t> submit_one(uint64_t phys, uint32_t len, bool writable);

    /// Publish a scatter-gather chain: @p out entries are device-read, @p in
    /// entries device-written, chained via NEXT in order out[0]..out[n_out-1]
    /// -> in[0]..in[n_in-1].  desc[0..n-1] fixed (serial caller).  virtio-blk
    /// uses this for its 3-desc request (header + data + status).
    cinux::lib::ErrorOr<uint16_t> submit_chain(const SgEntry* out, size_t n_out,
                                               const SgEntry* in, size_t n_in);

    /// Kick the queue (notify the device there is work in the avail ring).
    void kick() const;

    /// Poll until the used ring advances past @p avail_idx (from submit_*).
    /// @p out_len (optional) receives the bytes-written count.
    cinux::lib::ErrorOr<void> wait_completion(uint16_t avail_idx, uint32_t* out_len = nullptr);

    /// True if the device has posted a used entry since last_used_idx_.
    /// Non-blocking; for RX poll paths (virtio-net poll_rx).
    bool has_completion() const;

    /// Consume the next used entry (advance last_used_idx_) and return the
    /// byte count the device wrote.  Caller MUST check has_completion() first.
    uint32_t consume_completion();

    uint16_t size() const { return qsize_; }
    uint16_t last_used_idx() const { return last_used_idx_; }
    uint16_t avail_idx() const { return avail_idx_; }

private:
    void zero_tables();

    VirtIODevice*                dev_           = nullptr;
    uint16_t                     queue_index_   = 0;
    uint16_t                     qsize_         = 0;
    uint16_t                     notify_off_    = 0;  ///< from setup_queue, for kick()
    uint16_t                     avail_idx_     = 0;  ///< driver's next avail->idx
    uint16_t                     last_used_idx_ = 0;  ///< driver's last processed used->idx

    cinux::drivers::dma::DmaBuffer desc_buf_;   ///< desc table
    cinux::drivers::dma::DmaBuffer avail_buf_;  ///< avail ring
    cinux::drivers::dma::DmaBuffer used_buf_;   ///< used ring
};

}  // namespace cinux::drivers::virtio
