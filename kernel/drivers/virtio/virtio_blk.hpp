/**
 * @file kernel/drivers/virtio/virtio_blk.hpp
 * @brief VirtIOBlock -- virtio-blk device driver (F5-M2 batch 2)
 *
 * IBlockDevice adapter over a VirtIO Block device.  Submits 3-desc chains
 * (header + data + status) via VirtQueue::submit_chain, polls used for
 * completion (real interrupt deferred to batch 3).
 *
 * DmaBuffer layout (4 KiB): [header 16B][data count*512][status 1B].  count
 * limited to 7 sectors (3601 B < 4096) -- covers ext2 1 KiB blocks (count=2);
 * multi-page transfers are a follow-up (like NvmeBlockDevice's PRP list).
 *
 * SMP: a Spinlock serialises do_io from day one (NVMe 749e7db lesson -- single
 * DmaBuffer + queue must not overlap across CPUs).  Held across submit+wait;
 * batch 2 polls (bounded spin, no schedule), batch 3 will wake from ISR.
 *
 * Namespace: cinux::drivers::virtio
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>
#include <utility>

#include "kernel/drivers/block_device.hpp"
#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/drivers/virtio/virtio.hpp"
#include "kernel/drivers/virtio/virtqueue.hpp"

namespace cinux::drivers::virtio {

/// virtio-blk request type (virtio_blk_req.type, VIRTIO 1.1 §5.2.5).
namespace blk {
constexpr uint32_t T_IN    = 0;  // read (device -> driver)
constexpr uint32_t T_OUT   = 1;  // write (driver -> device)
constexpr uint32_t T_FLUSH = 4;
constexpr uint8_t  S_OK     = 0;  // status byte values
constexpr uint8_t  S_IOERR  = 1;
constexpr uint8_t  S_UNSUPP = 2;
}  // namespace blk

/**
 * @brief IBlockDevice adapter over a VirtIO Block device
 */
class VirtIOBlock : public cinux::drivers::IBlockDevice {
public:
    /// Bring up: allocate the request queue + DMA buffer.  @p capacity_sectors
    /// from VirtIODevice::device_cfg_read64(0) (the virtio-blk capacity field).
    static cinux::lib::ErrorOr<VirtIOBlock> create(VirtIODevice& dev, uint64_t capacity_sectors);

    VirtIOBlock(VirtIOBlock&&) noexcept            = default;
    VirtIOBlock& operator=(VirtIOBlock&&) noexcept = default;
    VirtIOBlock(const VirtIOBlock&)                = delete;
    VirtIOBlock& operator=(const VirtIOBlock&)     = delete;
    ~VirtIOBlock() override                        = default;

    cinux::lib::ErrorOr<void> read_blocks(uint64_t block, uint64_t count, void* buf) override;
    cinux::lib::ErrorOr<void> write_blocks(uint64_t block, uint64_t count,
                                           const void* buf) override;
    // flush(): inherit IBlockDevice default no-op (VIRTIO_BLK_T_FLUSH follow-up).

    uint64_t block_count() const override { return capacity_sectors_; }
    uint64_t block_size() const override { return 512; }

private:
    VirtIOBlock(VirtIODevice* dev, uint64_t capacity, dma::DmaBuffer&& buf, VirtQueue&& q);

    /// Submit a 3-desc blk request + kick + poll-wait.  dma_buf_ layout:
    /// [hdr 16][data count*512][status 1].  @p type = blk::T_IN / T_OUT.
    cinux::lib::ErrorOr<void> do_io(uint32_t type, uint64_t sector, uint16_t count);

    VirtIODevice*         dev_;
    uint64_t              capacity_sectors_;
    dma::DmaBuffer        dma_buf_;
    VirtQueue             request_queue_;
    cinux::proc::Spinlock lock_;
};

/// Production boot-disk accessor (set by main.cpp, read by init.cpp).
VirtIOBlock* virtio_block_device();
void         set_virtio_block_device(VirtIOBlock* bd);

}  // namespace cinux::drivers::virtio
