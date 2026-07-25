/**
 * @file kernel/net/loopback_device.hpp
 * @brief LoopbackDevice -- a software NetDevice (no MAC, no L2, zero-copy RX).
 *
 * The deterministic L3 testbed: a real NetDevice implemented entirely in
 * software, proving the stack is L2-agnostic and that a pure-software device
 * lives in kernel/net/ using the SAME interface as hardware drivers.  send_l3
 * copies the TX bytes into a queue slot; poll_rx hands that slot's pointer
 * directly to the dispatcher (zero-copy RX via BufferSink::recycle, which frees
 * the slot after dispatch).  send NEVER reenters dispatch -- a reply enqueued
 * during a handler's on_frame is drained on the NEXT poll() round (FOLD-A/B +
 * the reentrancy fix).
 *
 * @note An instance is ~kSlots*kBufSize bytes (8 * 1518 ~= 12 KB).  In the
 *       kernel, allocate it STATICALLY (the 16 KB kernel stack cannot hold one).
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>

#include "kernel/net/buffer.hpp"
#include "kernel/net/net_device.hpp"

namespace cinux::net {

class LoopbackDevice : public NetDevice, private BufferSink {
public:
    static constexpr uint32_t kSlots   = 8;
    static constexpr uint32_t kBufSize = 1518;  ///< max frame this device carries

    // --- NetDevice ---
    bool mac([[maybe_unused]] EthAddr& out) const override {
        return false;  // no L2 address
    }
    bool     has_ethernet_header() const override { return false; }
    uint32_t max_frame() const override { return kBufSize; }
    bool     supports_zerocopy() const override { return true; }

    /// @brief "Transmit" by copying into the next free queue slot (deferred --
    ///        drained on poll(), never reentrant).  @p next_hop / @p ethertype
    ///        are ignored (no L2; ethertype is derived on RX from the IPv4
    ///        version nibble).
    cinux::lib::ErrorOr<void> send_l3(const EthAddr& next_hop, uint16_t ethertype,
                                      const uint8_t* l3, uint32_t len) override;

    /// @brief Dequeue the next queued frame; hand its slot pointer (zero-copy).
    bool poll_rx(Packet& out) override;

    /// @brief Frames queued but not yet polled (diagnostic / test).
    uint32_t queued() const;

private:
    // --- BufferSink: return a polled slot to the free pool after dispatch. ---
    void recycle(const uint8_t* data) override;

    enum class State : uint8_t {
        Free,
        Queued,
        InFlight
    };

    uint8_t  storage_[kSlots][kBufSize];  ///< slot buffers (zero-copy RX source)
    uint32_t len_[kSlots]   = {};         ///< valid bytes per slot
    State    state_[kSlots] = {};         ///< Free / Queued / InFlight
    uint32_t fifo_[kSlots]  = {};         ///< indices of Queued slots (head -> tail)
    uint32_t head_          = 0;
    uint32_t tail_          = 0;
};

}  // namespace cinux::net
