/**
 * @file kernel/drivers/virtio/virtio_net.hpp
 * @brief VirtIONetDevice -- virtio-net NetDevice adapter (F5-M2 batch 4)
 *
 * RX/TX virtqueues over the VirtIO transport.  Fixed virtio_net_hdr (12 B, no
 * mergeable/CSUM/GSO).  RX: poll_rx supplies one writable buffer at a time +
 * lends the device-filled frame bytes via Packet.data (copy semantics -- the
 * buffer is re-supplied on the next poll, overwriting it).  TX: send_l3 builds
 * [virtio_net_hdr | EthHdr wire | l3] and submits one device-read chain.
 * Polling mode (real interrupt deferred to batch 5).
 *
 * Namespace: cinux::drivers::virtio
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstdint>
#include <utility>

#include "kernel/drivers/dma/dma_buffer.hpp"
#include "kernel/drivers/virtio/virtio.hpp"
#include "kernel/drivers/virtio/virtqueue.hpp"
#include "kernel/net/net_device.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::drivers::virtio {

namespace net {
constexpr uint32_t kHdrBytes  = 12;  ///< virtio_net_hdr fixed (no mergeable)
constexpr uint16_t kQueueSize = 64;
}  // namespace net

class VirtIONetDevice : public cinux::net::NetDevice {
public:
    static cinux::lib::ErrorOr<VirtIONetDevice> create(VirtIODevice& dev);

    VirtIONetDevice(VirtIONetDevice&&) noexcept            = default;
    VirtIONetDevice& operator=(VirtIONetDevice&&) noexcept = default;
    VirtIONetDevice(const VirtIONetDevice&)                = delete;
    VirtIONetDevice& operator=(const VirtIONetDevice&)     = delete;
    ~VirtIONetDevice() override                            = default;

    bool mac(cinux::net::EthAddr& out) const override {
        out = mac_;
        return true;
    }
    bool has_ethernet_header() const override { return true; }
    bool link_up() const override { return dev_->present(); }
    bool supports_zerocopy() const override { return false; }  // copy-based

    bool                      poll_rx(cinux::net::Packet& out) override;
    cinux::lib::ErrorOr<void> send_l3(const cinux::net::EthAddr& next_hop, uint16_t ethertype,
                                      const uint8_t* l3, uint32_t len) override;

    /// F5-M2 task 2: pre-fill one RX buffer (call once after create + DRIVER_OK,
    /// before any traffic).  Without this the first inbound frame (SLIRP ARP
    /// reply) arrives before poll_rx has supplied a buffer -> dropped -> ping
    /// stalls on ARP resolve.  poll_rx recycles + re-supplies on each consume.
    cinux::lib::ErrorOr<void> prime_rx();

private:
    VirtIONetDevice(VirtIODevice* dev, cinux::net::EthAddr mac, dma::DmaBuffer&& rx_buf,
                    dma::DmaBuffer&& tx_buf, VirtQueue&& rx, VirtQueue&& tx);

    VirtIODevice*         dev_;
    cinux::net::EthAddr   mac_;
    dma::DmaBuffer        rx_buf_;    // [virtio_net_hdr 12][frame <= 1514]
    dma::DmaBuffer        tx_buf_;    // [virtio_net_hdr 12][EthHdr wire 14][l3]
    VirtQueue             rx_queue_;  // queue 0
    VirtQueue             tx_queue_;  // queue 1
    bool                  rx_in_flight_ = false;
    cinux::proc::Spinlock lock_;
};

/// Programmatic accessor for the production VirtIO-net NIC (set from main.cpp
/// Step 21a3 once the adapter is created).  cinux::net::init() reads it to
/// attach the NIC to the L3 stack -- the SLIRP ping path (F5-M2 task 2).
VirtIONetDevice* virtio_net_device();
void             set_virtio_net_device(VirtIONetDevice* dev);

}  // namespace cinux::drivers::virtio
