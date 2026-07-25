/**
 * @file kernel/net/loopback_device.cpp
 * @brief LoopbackDevice -- queue send + zero-copy RX poll + recycle.
 *
 * Slot state machine: Free -> (send_l3) Queued -> (poll_rx) InFlight ->
 * (recycle after dispatch) Free.  send_l3 finds a Free slot and pushes its
 * index onto a FIFO; poll_rx pops the FIFO and hands the slot pointer directly
 * (zero-copy); recycle (BufferSink) returns the slot to Free.  send_l3 during a
 * handler's on_frame enqueues but does NOT recurse into dispatch -- the reply
 * is drained on the next poll() round (the reentrancy fix).
 *
 * Namespace: cinux::net
 */

#include "kernel/net/loopback_device.hpp"

#include <cstring>

namespace cinux::net {

cinux::lib::ErrorOr<void> LoopbackDevice::send_l3(const EthAddr& /*next_hop*/,
                                                  uint16_t /*ethertype*/, const uint8_t* l3,
                                                  uint32_t len) {
    if (len == 0) {
        return {};  // nothing to loop
    }
    if (len > kBufSize) {
        return cinux::lib::Error::InvalidArgument;
    }
    // Find a free slot (linear scan; kSlots is tiny).
    uint32_t s = 0;
    while (s < kSlots && state_[s] != State::Free) {
        ++s;
    }
    if (s == kSlots) {
        return cinux::lib::Error::OutOfMemory;  // queue full -> caller should drain (poll)
    }
    std::memcpy(storage_[s], l3, len);
    len_[s]      = len;
    state_[s]    = State::Queued;
    fifo_[tail_] = s;
    tail_        = (tail_ + 1) % kSlots;
    return {};
}

bool LoopbackDevice::poll_rx(Packet& out) {
    if (head_ == tail_) {
        return false;  // empty FIFO
    }
    const uint32_t s = fifo_[head_];
    head_            = (head_ + 1) % kSlots;
    // state_[s] is Queued (the FIFO only holds Queued slots).
    state_[s]        = State::InFlight;
    out.data         = storage_[s];
    out.len          = len_[s];
    out.sink         = this;  // zero-copy RX: hand the slot; recycle() returns it.
    return true;
}

void LoopbackDevice::recycle(const uint8_t* data) {
    // Recover the slot index from the pointer (each slot is kBufSize-aligned
    // within storage_).
    const uintptr_t base = reinterpret_cast<uintptr_t>(&storage_[0][0]);
    const uintptr_t off  = reinterpret_cast<uintptr_t>(data) - base;
    const uint32_t  s    = static_cast<uint32_t>(off / kBufSize);
    if (s < kSlots && state_[s] == State::InFlight) {
        state_[s] = State::Free;
    }
}

uint32_t LoopbackDevice::queued() const {
    // FIFO occupancy (head -> tail, modulo).
    return (tail_ + kSlots - head_) % kSlots;
}

}  // namespace cinux::net
