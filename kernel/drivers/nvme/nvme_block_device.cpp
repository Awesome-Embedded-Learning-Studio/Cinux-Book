/**
 * @file kernel/drivers/nvme/nvme_block_device.cpp
 * @brief NvmeBlockDevice implementation (F5-M3 batch 4c)
 */

#include "nvme_block_device.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::drivers::nvme {

namespace {

// One page holds up to 4096/lba_size LBAs (8 at 512 B).  Transfers larger than
// this are rejected rather than chunked; a PRP list for multi-page transfers is
// a follow-up (batch 4c minimal synchronous path mirrors AHCIBlockDevice).
constexpr uint64_t kDmaBufferSize = 4096;

}  // namespace

cinux::lib::ErrorOr<NvmeBlockDevice> NvmeBlockDevice::create(NvmeController& ctrl, uint32_t nsid,
                                                             uint64_t capacity_blocks,
                                                             uint64_t lba_size) {
    auto buf = dma::g_dma_pool.alloc(kDmaBufferSize);
    if (!buf.ok()) {
        return buf.error();
    }
    return NvmeBlockDevice(&ctrl, nsid, capacity_blocks, lba_size, std::move(buf.value()));
}

NvmeBlockDevice::NvmeBlockDevice(NvmeController* ctrl, uint32_t nsid, uint64_t capacity_blocks,
                                 uint64_t lba_size, dma::DmaBuffer&& dma_buf)
    : ctrl_(ctrl),
      nsid_(nsid),
      capacity_blocks_(capacity_blocks),
      lba_size_(lba_size),
      dma_buf_(std::move(dma_buf)) {}

cinux::lib::ErrorOr<void> NvmeBlockDevice::read_blocks(uint64_t block, uint64_t count, void* buf) {
    if (count == 0) {
        return {};
    }
    auto g = lock_.guard();  // SMP: serialise dma_buf_ + NVMe IO across CPUs
    if (!dma_buf_.valid()) {
        return cinux::lib::Error::IOError;  // no DMA buffer (create-time alloc failed)
    }
    const uint64_t bytes = count * lba_size_;
    if (bytes > dma_buf_.size()) {
        return cinux::lib::Error::InvalidArgument;  // transfer exceeds adapter buffer
    }
    // NVMe NLB field is 16-bit; single-page transfers keep count well under that.
    auto r = ctrl_->read_blocks(nsid_, block, static_cast<uint16_t>(count), dma_buf_);
    if (!r.ok()) {
        return r.error();
    }
    memcpy(buf, dma_buf_.virt(), static_cast<std::size_t>(bytes));
    return {};
}

cinux::lib::ErrorOr<void> NvmeBlockDevice::write_blocks(uint64_t block, uint64_t count,
                                                        const void* buf) {
    if (count == 0) {
        return {};
    }
    auto g = lock_.guard();  // SMP: serialise dma_buf_ + NVMe IO across CPUs
    if (!dma_buf_.valid()) {
        return cinux::lib::Error::IOError;
    }
    const uint64_t bytes = count * lba_size_;
    if (bytes > dma_buf_.size()) {
        return cinux::lib::Error::InvalidArgument;
    }
    memcpy(dma_buf_.virt(), buf, static_cast<std::size_t>(bytes));
    auto r = ctrl_->write_blocks(nsid_, block, static_cast<uint16_t>(count), dma_buf_);
    if (!r.ok()) {
        return r.error();
    }
    return {};
}

namespace {
/// Production boot-disk singleton (set by main.cpp Step 21a, read by init.cpp).
NvmeBlockDevice* g_nvme_blk = nullptr;
}  // namespace

NvmeBlockDevice* nvme_block_device() { return g_nvme_blk; }
void             set_nvme_block_device(NvmeBlockDevice* bd) { g_nvme_blk = bd; }

}  // namespace cinux::drivers::nvme
