#include "kernel/driver/block/block_queue.hpp"

#include <stdint.h>

#include "cinux/assert.hpp"
#include "kernel/driver/block/block_config.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {

bool BlockQueue::read(Lba first, BlockSpan span, uint8_t* destination) {
    const unsigned long kSlot = enqueue(first, span, destination, RequestKind::kRead);
    if (kSlot == kBlockQueueDepth) {
        return false;
    }
    return await(kSlot);
}

bool BlockQueue::write(Lba first, BlockSpan span, const uint8_t* source) {
    const unsigned long kSlot = enqueue(
        first, span, const_cast<void*>(static_cast<const void*>(source)), RequestKind::kWrite);
    if (kSlot == kBlockQueueDepth) {
        return false;
    }
    return await(kSlot);
}

bool BlockQueue::flush() {
    const unsigned long kSlot = enqueue(Lba{}, BlockSpan{}, nullptr, RequestKind::kFlush);
    if (kSlot == kBlockQueueDepth) {
        return false;
    }
    return await(kSlot);
}

unsigned long BlockQueue::enqueue(Lba first, BlockSpan span, void* data, RequestKind kind) {
    const proc::SpinGuard kGuard(lock_);
    const unsigned long   kSlot = pool_.insert_default();
    if (kSlot == kBlockQueueDepth) {
        return kSlot;
    }
    BlockRequest& request = *pool_.get(kSlot);
    request.first         = first;
    request.span          = span;
    request.data          = data;
    request.kind          = kind;
    request.done.rearm();
    pending_.push(static_cast<unsigned int>(kSlot));
    return kSlot;
}

bool BlockQueue::await(unsigned long slot) {
    for (;;) {
        bool duty = false;
        {
            const proc::SpinGuard kGuard(lock_);
            const BlockRequest&   request = *pool_.get(slot);
            if (request.done.finished()) {
                const bool kMoved = request.done.success();
                cinux::base::safety::Check(pool_.erase(slot),
                                           "block request slot vanished before reclaim");
                return kMoved;
            }
            if (!duty_taken_) {
                duty_taken_ = true;
                duty        = true;
            }
        }
        if (duty) {
            drive();
            const proc::SpinGuard kGuard(lock_);
            if (pending_.count() == 0) {
                duty_taken_ = false;
            }
            continue;
        }
        BlockRequest& request = *pool_.get(slot);
        const bool    kMoved  = request.done.wait();
        {
            const proc::SpinGuard kGuard(lock_);
            cinux::base::safety::Check(pool_.erase(slot),
                                       "block request slot vanished before reclaim");
        }
        return kMoved;
    }
}

void BlockQueue::drive() {
    for (;;) {
        unsigned int slot_id = 0;
        {
            const proc::SpinGuard kGuard(lock_);
            if (!pending_.pop(slot_id)) {
                return;
            }
        }
        BlockRequest& request = *pool_.get(slot_id);
        bool          moved   = false;
        switch (request.kind) {
        case RequestKind::kRead:
            moved = device_.read_blocks(request.first, request.span,
                                        static_cast<uint8_t*>(request.data));
            break;
        case RequestKind::kWrite:
            moved = device_.write_blocks(request.first, request.span,
                                         static_cast<const uint8_t*>(request.data));
            break;
        case RequestKind::kFlush:
            moved = device_.flush();
            break;
        }
        request.done.finish(moved);
    }
}

}  // namespace cinux::driver
