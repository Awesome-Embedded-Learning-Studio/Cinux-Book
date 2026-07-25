/**
 * @file kernel/drivers/virtio/virtio_blk.cpp
 * @brief VirtIOBlock implementation (F5-M2 batch 2)
 *
 * 3-desc chain per request: header (device-read) + data (dir per type) + status
 * (device-written).  Single DmaBuffer holds all three; count limited so the
 * data region + 17 B overhead fits in 4 KiB.
 */

#include "kernel/drivers/virtio/virtio_blk.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::drivers::virtio {

namespace {
constexpr uint64_t kDmaBufferSize = 4096;
constexpr uint64_t kHeaderBytes   = 16;   // virtio_blk_req header (type+reserved+sector)
constexpr uint64_t kStatusBytes   = 1;
constexpr uint16_t kMaxCount = 7;          // (4096 - 17) / 512
constexpr uint16_t kQueueSize = 64;

// virtio_blk_req header (16 bytes): type(4) + reserved(4) + sector(8).
struct BlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
static_assert(sizeof(BlkReqHeader) == kHeaderBytes, "BlkReqHeader must be 16 bytes");
}  // namespace

cinux::lib::ErrorOr<VirtIOBlock> VirtIOBlock::create(VirtIODevice& dev,
                                                     uint64_t capacity_sectors) {
    auto buf = dma::g_dma_pool.alloc(kDmaBufferSize);
    if (!buf.ok()) {
        return buf.error();
    }
    VirtQueue q;
    auto qr = q.init(&dev, 0, kQueueSize);
    if (!qr.ok()) {
        return qr.error();
    }
    return VirtIOBlock(&dev, capacity_sectors, std::move(buf.value()), std::move(q));
}

VirtIOBlock::VirtIOBlock(VirtIODevice* dev, uint64_t capacity, dma::DmaBuffer&& buf,
                         VirtQueue&& q)
    : dev_(dev),
      capacity_sectors_(capacity),
      dma_buf_(std::move(buf)),
      request_queue_(std::move(q)) {}

cinux::lib::ErrorOr<void> VirtIOBlock::do_io(uint32_t type, uint64_t sector, uint16_t count) {
    const uint64_t data_bytes = static_cast<uint64_t>(count) * 512;
    const uint64_t data_off   = kHeaderBytes;
    const uint64_t status_off = kHeaderBytes + data_bytes;

    // Build the request header at dma_buf_ offset 0.
    auto* hdr          = static_cast<volatile BlkReqHeader*>(dma_buf_.virt());
    hdr->type          = type;
    hdr->reserved      = 0;
    hdr->sector        = sector;
    // Status byte (device-written): poison before submit so a no-op completion
    // is detectable.
    auto* status       = static_cast<volatile uint8_t*>(dma_buf_.virt()) + status_off;
    *status            = 0xff;

    const uint64_t base = dma_buf_.phys();
    // out (device-read): header (+ data if write).  in (device-written):
    // (+ data if read) + status.
    SgEntry out[2];
    size_t  n_out = 1;
    out[0] = {base, static_cast<uint32_t>(kHeaderBytes)};
    if (type == blk::T_OUT) {
        out[1] = {base + data_off, static_cast<uint32_t>(data_bytes)};
        n_out  = 2;
    }
    SgEntry in[2];
    size_t  n_in;
    if (type == blk::T_IN) {
        in[0] = {base + data_off, static_cast<uint32_t>(data_bytes)};
        in[1] = {base + status_off, static_cast<uint32_t>(kStatusBytes)};
        n_in  = 2;
    } else {
        in[0] = {base + status_off, static_cast<uint32_t>(kStatusBytes)};
        n_in  = 1;
    }

    auto sr = request_queue_.submit_chain(out, n_out, in, n_in);
    if (!sr.ok()) {
        return sr.error();
    }
    request_queue_.kick();
    auto wr = request_queue_.wait_completion(sr.value());
    if (!wr.ok()) {
        return wr.error();
    }
    if (*status != blk::S_OK) {
        cinux::lib::kprintf("[VirtIO-blk] io status=0x%x (type=%u sector=%llu count=%u)\n",
                            *status, type, static_cast<unsigned long long>(sector), count);
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<void> VirtIOBlock::read_blocks(uint64_t block, uint64_t count, void* buf) {
    if (count == 0) {
        return {};
    }
    auto g = lock_.guard();  // SMP: serialise dma_buf_ + queue across CPUs
    if (count > kMaxCount) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto r = do_io(blk::T_IN, block, static_cast<uint16_t>(count));
    if (!r.ok()) {
        return r.error();
    }
    const uint64_t data_bytes = count * 512;
    memcpy(buf, static_cast<uint8_t*>(dma_buf_.virt()) + kHeaderBytes,
           static_cast<std::size_t>(data_bytes));
    return {};
}

cinux::lib::ErrorOr<void> VirtIOBlock::write_blocks(uint64_t block, uint64_t count,
                                                    const void* buf) {
    if (count == 0) {
        return {};
    }
    auto g = lock_.guard();
    if (count > kMaxCount) {
        return cinux::lib::Error::InvalidArgument;
    }
    const uint64_t data_bytes = count * 512;
    memcpy(static_cast<uint8_t*>(dma_buf_.virt()) + kHeaderBytes, buf,
           static_cast<std::size_t>(data_bytes));
    return do_io(blk::T_OUT, block, static_cast<uint16_t>(count));
}

namespace {
VirtIOBlock* g_virtio_blk = nullptr;
}  // namespace

VirtIOBlock* virtio_block_device() { return g_virtio_blk; }
void         set_virtio_block_device(VirtIOBlock* bd) { g_virtio_blk = bd; }

}  // namespace cinux::drivers::virtio

// ============================================================
// MSI-X interrupt handler (F5-M2 batch 3, vector 0x42 kVirtioBlkIrqVector)
// ============================================================
// Fires when the device posts a used-ring entry (request completion).  Counts
// only; the polling path in wait_completion independently observes used->idx,
// so this handler does NOT need to wake a wait-queue (production can swap the
// spin for prepare_to_wait/schedule_blocked here -- the ISR is the wake seam).
// NO schedule() in ISR -- sti-in-syscall #DF (see sys-ping-df).  EOI by the
// ISR_IRQ asm stub.
namespace cinux::arch {
struct InterruptFrame;  // fwd decl for the C handler signature
}

volatile uint64_t g_virtio_blk_irq_count = 0;
extern "C" void virtio_blk_irq_handler(cinux::arch::InterruptFrame* /*frame*/) {
    g_virtio_blk_irq_count = g_virtio_blk_irq_count + 1;
}
