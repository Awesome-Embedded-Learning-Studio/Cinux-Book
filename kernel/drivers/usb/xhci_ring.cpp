/**
 * @file kernel/drivers/usb/xhci_ring.cpp
 * @brief xHCI TRB ring mechanics (pure: operates on a Trb array, no DMA/VMM)
 *
 * Producer (TrbRing) and consumer (EventRing) cycle-bit math.  Pure so it
 * links and runs identically in the kernel (DmaBuffer-backed) and in host unit
 * tests (stack array).  Kernel DmaBuffer wiring + CRCR/ERSTBA programming land
 * in Batch 2B.
 *
 * Namespace: cinux::drivers::usb
 */

#include "xhci_ring.hpp"

namespace cinux::drivers::usb {

// ============================================================
// TrbRing (producer)
// ============================================================

void TrbRing::write_link() {
    Trb* link       = &storage_[slots_];
    link->parameter = base_phys_;
    link->status    = 0;
    link->control   = trb_control(TrbType::kLink, kToggleCycle) | (pcs_ ? kCycleBit : 0);
}

void TrbRing::init(Trb* storage, uint32_t slots, uint64_t base_phys) {
    storage_   = storage;
    slots_     = slots;
    base_phys_ = base_phys;
    enqueue_   = 0;
    pcs_       = true;
    write_link();  // initial Link TRB (cycle = PCS = 1)
}

void TrbRing::enqueue(uint64_t parameter, uint32_t status, uint32_t control) {
    Trb* t       = &storage_[enqueue_];
    t->parameter = parameter;
    t->status    = status;
    t->control   = control | (pcs_ ? kCycleBit : 0);
    ++enqueue_;
    if (enqueue_ == slots_) {
        write_link();  // rewrite Link TRB (cycle = current PCS)
        enqueue_ = 0;
        pcs_     = !pcs_;  // flip PCS for the next lap
    }
}

// ============================================================
// EventRing (consumer)
// ============================================================

void EventRing::init(Trb* storage, uint32_t size) {
    storage_ = storage;
    size_    = size;
    dequeue_ = 0;
    ccs_     = true;
}

bool EventRing::dequeue(Trb& out) {
    Trb*       t     = &storage_[dequeue_];
    const bool cycle = (t->control & kCycleBit) != 0;
    if (cycle != ccs_) {
        return false;  // not owned by the consumer -> ring empty
    }
    out.parameter = t->parameter;  // member-wise copy (volatile source)
    out.status    = t->status;
    out.control   = t->control;
    ++dequeue_;
    if (dequeue_ == size_) {
        dequeue_ = 0;
        ccs_     = !ccs_;  // wrapped: consumer flips its cycle state
    }
    return true;
}

}  // namespace cinux::drivers::usb
