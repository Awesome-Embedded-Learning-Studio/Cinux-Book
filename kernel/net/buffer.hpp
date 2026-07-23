/**
 * @file kernel/net/buffer.hpp
 * @brief RX buffer abstraction -- the unit that crosses the device/stack seam.
 *
 * The ONE subtle piece of the net stack.  A received frame is a Packet:
 * bytes + an opaque recycle hook.  The hook is NIC-agnostic (BufferSink lives
 * in kernel/net/ and never names dma::DmaBuffer), so the same dispatch path
 * serves copy devices (e1000: sink == nullptr, bytes live in device-internal
 * scratch, recycle is a no-op) and zero-copy devices (loopback / future
 * virtio: sink == the device, recycle returns the buffer to its ring/pool).
 *
 * Lifetime contract (the three red-team hazards, resolved):
 *  - Use-after-free: a Packet is valid only inside ProtocolHandler::on_frame.
 *    NetStack::poll() recycles via a scope guard on EVERY exit path right
 *    after dispatch returns.  A handler that needs bytes past return MUST copy
 *    them into its own storage (borrow, never retain).
 *  - Drop/leak: the scope guard fires whether the frame was handled, had no
 *    matching handler, was a runt, or failed L2 parse -- so a device-owned
 *    buffer is never leaked.
 *  - Reentrancy: a handler calling stack.send_l3() during on_frame (ARP/ICMP
 *    reply immediately) must NOT reenter dispatch on the same buffer.  A
 *    software device (loopback) defers: send enqueues into a multi-slot RX
 *    queue, dispatched on the NEXT poll() -- see loopback_device.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <cstdint>

namespace cinux::net {

/// @brief Owner-side hook to release a device-owned buffer after dispatch.
///
/// Implemented by a zero-copy device that hands the stack a borrowed buffer
/// (loopback RX-queue slot, future virtio DMA page).  A copy device leaves
/// Packet::sink == nullptr and needs no BufferSink.  NIC-agnostic: the type
/// lives in kernel/net/ and references no dma/e1000/arch header.
class BufferSink {
public:
    virtual ~BufferSink() = default;

    /// @brief Return @p data's backing store to the device (advance ring / free
    ///        the queue slot).  Called exactly once per received Packet, after
    ///        dispatch, by NetStack::poll()'s scope guard.
    virtual void recycle(const uint8_t* data) = 0;
};

/// @brief A received frame handed to the dispatcher.
///
/// @note Non-owning: @p data is valid only until the dispatch returns and the
///       scope guard calls sink->recycle() (or, for a copy device with
///       sink==nullptr, until the device's next poll_rx overwrites its scratch).
struct Packet {
    const uint8_t* data = nullptr;  ///< frame bytes (L2 header + payload)
    uint32_t       len  = 0;        ///< valid byte count
    /// Recycle hook, or nullptr for a device-internal scratch (copy RX).  When
    /// non-null, NetStack::poll() calls sink->recycle(data) after dispatch.
    BufferSink*    sink = nullptr;
};

}  // namespace cinux::net
