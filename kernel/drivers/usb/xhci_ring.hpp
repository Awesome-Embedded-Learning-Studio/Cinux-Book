/**
 * @file kernel/drivers/usb/xhci_ring.hpp
 * @brief xHCI TRB rings: producer (command/transfer) + consumer (event)
 *
 * Two ring flavours share the TRB array + cycle-bit idea but wrap differently:
 *   - TrbRing  (command + transfer rings): the HOST is the producer.  A Link
 *     TRB at the end closes the ring and toggles the Producer Cycle State
 *     (PCS) on every lap.  storage holds `slots + 1` TRBs (slots usable + the
 *     Link TRB).
 *   - EventRing (event ring): the CONTROLLER is the producer.  There is NO
 *     Link TRB -- wrap is by ERST segment size, and the Consumer Cycle State
 *     (CCS) flips when the dequeue pointer wraps past the last slot.  storage
 *     holds exactly `size` TRBs.
 *
 * Both operate on a caller-owned Trb array, so the ring MATH is pure and
 * host-testable; the kernel wraps a DmaBuffer backing + programs phys into
 * CRCR / ERSTBA (Batch 2B).
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

#include "xhci_trb.hpp"

namespace cinux::drivers::usb {

// ============================================================
// Producer ring (command + transfer)
// ============================================================

class TrbRing {
public:
    /// Bind the ring to @p storage (`slots + 1` TRBs) and write the initial
    /// Link TRB.  @p base_phys is the bus address programmed into CRCR and the
    /// Link TRB's parameter.
    void init(Trb* storage, uint32_t slots, uint64_t base_phys);

    /// Enqueue one TRB: writes parameter/status/control(type+flags, no cycle
    /// bit), sets the cycle bit = PCS, and advances.  On reaching the last
    /// usable slot it rewrites the Link TRB, wraps to 0, and flips PCS.
    void enqueue(uint64_t parameter, uint32_t status, uint32_t control);

    uint32_t enqueue_index() const { return enqueue_; }
    bool     producer_cycle() const { return pcs_; }
    Trb*     base() const { return storage_; }  // diagnostic access

private:
    void write_link();

    Trb*     storage_   = nullptr;
    uint32_t slots_     = 0;
    uint64_t base_phys_ = 0;
    uint32_t enqueue_   = 0;
    bool     pcs_       = true;
};

// ============================================================
// Consumer ring (event)
// ============================================================

class EventRing {
public:
    /// Bind to @p storage (`size` TRBs, no Link TRB).
    void init(Trb* storage, uint32_t size);

    /// If the TRB at the dequeue slot has its cycle bit == CCS, copy it to
    /// @p out, advance (wrap + flip CCS at the end), and return true.
    /// Returns false when the ring is empty (cycle mismatch).
    bool dequeue(Trb& out);

    uint32_t dequeue_index() const { return dequeue_; }
    bool     consumer_cycle() const { return ccs_; }
    Trb*     base() const { return storage_; }  // diagnostic access

private:
    Trb*     storage_ = nullptr;
    uint32_t size_    = 0;
    uint32_t dequeue_ = 0;
    bool     ccs_     = true;
};

}  // namespace cinux::drivers::usb
