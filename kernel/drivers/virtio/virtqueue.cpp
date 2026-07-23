/**
 * @file kernel/drivers/virtio/virtqueue.cpp
 * @brief VirtQueue -- split virtqueue implementation (F5-M2 batch 1+2)
 *
 * submit_one / submit_chain publish to desc[0..n-1] from a fixed start (serial
 * caller).  wait_completion polls the used ring's free-running idx.
 */

#include "kernel/drivers/virtio/virtqueue.hpp"

#include <stdint.h>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers::virtio {

namespace {
constexpr uint32_t kPollIters = 1000000;  ///< device-completion spin budget (no IRQ yet)

void zero_dma(void* p, uint64_t bytes) {
    auto b = static_cast<volatile uint64_t*>(p);
    for (uint64_t i = 0; i < bytes / 8; ++i) {
        b[i] = 0;
    }
}
}  // namespace

void VirtQueue::zero_tables() {
    zero_dma(desc_buf_.virt(), desc_buf_.size());
    zero_dma(avail_buf_.virt(), avail_buf_.size());
    zero_dma(used_buf_.virt(), used_buf_.size());
}

cinux::lib::ErrorOr<void> VirtQueue::init(VirtIODevice* dev, uint16_t queue_index, uint16_t qsize) {
    dev_         = dev;
    queue_index_ = queue_index;
    qsize_       = qsize;

    auto dr = cinux::drivers::dma::g_dma_pool.alloc(desc_table_bytes(qsize));
    auto ar = cinux::drivers::dma::g_dma_pool.alloc(avail_ring_bytes(qsize));
    auto ur = cinux::drivers::dma::g_dma_pool.alloc(used_ring_bytes(qsize));
    if (!dr.ok() || !ar.ok() || !ur.ok()) {
        cinux::lib::kprintf("[VirtQueue] DMA alloc failed (qsz=%u)\n", qsize);
        return cinux::lib::Error::OutOfMemory;
    }
    desc_buf_  = std::move(dr.value());
    avail_buf_ = std::move(ar.value());
    used_buf_  = std::move(ur.value());
    zero_tables();

    // desc/avail/used zeroed above.  submit_one/submit_chain use fixed desc[0..n-1]
    // (serialised by the caller via kick + wait_completion), so no free-list.
    avail_idx_     = 0;
    last_used_idx_ = 0;

    auto sr = dev_->setup_queue(queue_index, qsize, desc_buf_.phys(), avail_buf_.phys(),
                                used_buf_.phys());
    if (!sr.ok()) {
        return sr.error();
    }
    notify_off_ = sr.value();
    dev_->enable_queue(queue_index);
    return {};
}

cinux::lib::ErrorOr<uint16_t> VirtQueue::submit_one(uint64_t phys, uint32_t len, bool writable) {
    // Single desc at fixed index 0 (serial caller guarantees no overlap with
    // a previous submission that has already been waited on).
    auto* desc    = static_cast<volatile VqDesc*>(desc_buf_.virt());
    desc[0].addr  = phys;
    desc[0].len   = len;
    desc[0].flags = writable ? DescFlag::WRITE : 0;

    auto*          avail     = static_cast<volatile uint8_t*>(avail_buf_.virt());
    const uint16_t slot      = static_cast<uint16_t>(avail_idx_ % qsize_);
    auto*          ring      = reinterpret_cast<volatile uint16_t*>(avail + AvailOff::RING);
    ring[slot]               = 0;
    const uint16_t published = static_cast<uint16_t>(avail_idx_ + 1);
    *reinterpret_cast<volatile uint16_t*>(avail + AvailOff::IDX) = published;
    avail_idx_                                                   = published;
    return published;
}

cinux::lib::ErrorOr<uint16_t> VirtQueue::submit_chain(const SgEntry* out, size_t n_out,
                                                      const SgEntry* in, size_t n_in) {
    // desc[0..n_out+n_in-1]: out entries device-read (NEXT), in entries
    // device-written (WRITE + NEXT except last).  Fixed start; caller
    // serialises via kick + wait_completion.
    auto*        desc  = static_cast<volatile VqDesc*>(desc_buf_.virt());
    const size_t total = n_out + n_in;
    if (total == 0 || total > qsize_) {
        return cinux::lib::Error::InvalidArgument;
    }
    size_t d = 0;
    for (size_t i = 0; i < n_out; ++i) {
        const bool last = (d + 1 == total);  // F5-M2 task 2: last desc carries no NEXT
        desc[d].addr    = out[i].phys;
        desc[d].len     = out[i].len;
        desc[d].flags   = last ? 0 : DescFlag::NEXT;
        desc[d].next    = last ? 0 : static_cast<uint16_t>(d + 1);
        ++d;
    }
    for (size_t i = 0; i < n_in; ++i) {
        const bool last = (d + 1 == total);
        desc[d].addr    = in[i].phys;
        desc[d].len     = in[i].len;
        desc[d].flags   = DescFlag::WRITE | (last ? 0 : DescFlag::NEXT);
        desc[d].next    = last ? 0 : static_cast<uint16_t>(d + 1);
        ++d;
    }

    auto*          avail     = static_cast<volatile uint8_t*>(avail_buf_.virt());
    const uint16_t slot      = static_cast<uint16_t>(avail_idx_ % qsize_);
    auto*          ring      = reinterpret_cast<volatile uint16_t*>(avail + AvailOff::RING);
    ring[slot]               = 0;  // head = desc[0]
    const uint16_t published = static_cast<uint16_t>(avail_idx_ + 1);
    *reinterpret_cast<volatile uint16_t*>(avail + AvailOff::IDX) = published;
    avail_idx_                                                   = published;
    return published;
}

void VirtQueue::kick() const {
    dev_->notify_queue(queue_index_, notify_off_);
}

cinux::lib::ErrorOr<void> VirtQueue::wait_completion(uint16_t target, uint32_t* out_len) {
    auto* used = static_cast<volatile uint8_t*>(used_buf_.virt());
    for (uint32_t i = 0; i < kPollIters; ++i) {
        const uint16_t used_idx = *reinterpret_cast<volatile uint16_t*>(used + UsedOff::IDX);
        if (used_idx == target) {
            if (out_len != nullptr) {
                const uint16_t slot = static_cast<uint16_t>(target - 1);
                auto*          ring = reinterpret_cast<volatile VqUsedElem*>(used + UsedOff::RING);
                *out_len            = ring[slot % qsize_].len;
            }
            last_used_idx_ = target;
            return {};
        }
    }
    cinux::lib::kprintf("[VirtQueue] wait_completion timeout (target=%u used=%u)\n", target,
                        *reinterpret_cast<volatile uint16_t*>(used + UsedOff::IDX));
    return cinux::lib::Error::TimedOut;
}

bool VirtQueue::has_completion() const {
    auto*          used     = static_cast<volatile uint8_t*>(used_buf_.virt());
    const uint16_t used_idx = *reinterpret_cast<volatile uint16_t*>(used + UsedOff::IDX);
    return used_idx != last_used_idx_;
}

uint32_t VirtQueue::consume_completion() {
    auto*          used = static_cast<volatile uint8_t*>(used_buf_.virt());
    const uint16_t slot = static_cast<uint16_t>(last_used_idx_ % qsize_);
    auto*          ring = reinterpret_cast<volatile VqUsedElem*>(used + UsedOff::RING);
    const uint32_t len  = ring[slot].len;
    last_used_idx_      = static_cast<uint16_t>(last_used_idx_ + 1);
    return len;
}

}  // namespace cinux::drivers::virtio
