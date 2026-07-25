/**
 * @file kernel/drivers/virtio/virtio_net.cpp
 * @brief VirtIONetDevice implementation (F5-M2 batch 4)
 *
 * Fixed virtio_net_hdr (12 B).  RX supplies one writable buffer at a time
 * (poll_rx); TX submits [hdr | EthHdr wire | l3] as one device-read chain.
 */

#include "kernel/drivers/virtio/virtio_net.hpp"

#include <cstddef>
#include <cstdint>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::drivers::virtio {

namespace {
constexpr uint64_t kRxBufSize = net::kHdrBytes + 1514;
constexpr uint64_t kTxBufSize = net::kHdrBytes + 14 + 1514;

struct VirtioNetHdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};
static_assert(sizeof(VirtioNetHdr) == net::kHdrBytes, "VirtioNetHdr must be 12 bytes");
}  // namespace

cinux::lib::ErrorOr<VirtIONetDevice> VirtIONetDevice::create(VirtIODevice& dev) {
    auto rx_buf = dma::g_dma_pool.alloc(kRxBufSize);
    auto tx_buf = dma::g_dma_pool.alloc(kTxBufSize);
    if (!rx_buf.ok() || !tx_buf.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    VirtQueue rx;
    VirtQueue tx;
    auto      rr = rx.init(&dev, 0, net::kQueueSize);  // RX = queue 0
    if (!rr.ok()) {
        return rr.error();
    }
    auto tr = tx.init(&dev, 1, net::kQueueSize);  // TX = queue 1
    if (!tr.ok()) {
        return tr.error();
    }
    // MAC: device_cfg offset 0 (6 bytes), valid once VIRTIO_NET_F_MAC is on
    // (a default modern device sets it).  Read unconditionally; a zero MAC is
    // tolerated (the stack still routes via the resolved next_hop MAC).
    cinux::net::EthAddr mac{};
    for (uint32_t i = 0; i < 6; ++i) {
        mac.oct[i] = dev.device_cfg_read8(i);
    }
    return VirtIONetDevice(&dev, mac, std::move(rx_buf.value()), std::move(tx_buf.value()),
                           std::move(rx), std::move(tx));
}

VirtIONetDevice::VirtIONetDevice(VirtIODevice* dev, cinux::net::EthAddr mac,
                                 dma::DmaBuffer&& rx_buf, dma::DmaBuffer&& tx_buf, VirtQueue&& rx,
                                 VirtQueue&& tx)
    : dev_(dev),
      mac_(mac),
      rx_buf_(std::move(rx_buf)),
      tx_buf_(std::move(tx_buf)),
      rx_queue_(std::move(rx)),
      tx_queue_(std::move(tx)) {}

cinux::lib::ErrorOr<void> VirtIONetDevice::prime_rx() {
    auto g = lock_.guard();
    if (rx_in_flight_) {
        return {};  // already primed
    }
    auto sr = rx_queue_.submit_one(rx_buf_.phys(), static_cast<uint32_t>(kRxBufSize), true);
    if (!sr.ok()) {
        return sr.error();
    }
    rx_queue_.kick();
    rx_in_flight_ = true;
    return {};
}

bool VirtIONetDevice::poll_rx(cinux::net::Packet& out) {
    auto g = lock_.guard();
    if (!rx_in_flight_) {
        // Supply one writable buffer; the device fills virtio_net_hdr + frame
        // on receipt and writes it back via the used ring.
        auto sr = rx_queue_.submit_one(rx_buf_.phys(), static_cast<uint32_t>(kRxBufSize), true);
        if (!sr.ok()) {
            return false;
        }
        rx_queue_.kick();
        rx_in_flight_ = true;
        return false;  // next poll checks for completion
    }
    if (!rx_queue_.has_completion()) {
        return false;
    }
    const uint32_t len = rx_queue_.consume_completion();
    rx_in_flight_      = false;
    if (len < net::kHdrBytes) {
        return false;  // malformed (device wrote < hdr)
    }
    uint32_t frame_len = len - net::kHdrBytes;
    if (frame_len > 1514) {
        frame_len = 1514;
    }
    // Borrow the device buffer for this dispatch; copy semantics (sink=nullptr)
    // -- the buffer is re-supplied on the next poll, overwriting it.
    out.data = static_cast<const uint8_t*>(rx_buf_.virt()) + net::kHdrBytes;
    out.len  = frame_len;
    out.sink = nullptr;
    return true;
}

cinux::lib::ErrorOr<void> VirtIONetDevice::send_l3(const cinux::net::EthAddr& next_hop,
                                                   uint16_t ethertype, const uint8_t* l3,
                                                   uint32_t len) {
    auto  g     = lock_.guard();
    auto* base  = static_cast<uint8_t*>(tx_buf_.virt());
    // virtio_net_hdr at 0..11: zero (no csum/gso offload).
    auto* hdr   = reinterpret_cast<VirtioNetHdr*>(base);
    *hdr        = {};
    // Ethernet II wire frame at +12: dst(6) + src(6) + ethertype(2 big-endian)
    // + l3 payload.  Built byte-wise (EthHdr is a HOST-order view, NOT a wire
    // overlay -- see net_types.hpp).
    auto* frame = base + net::kHdrBytes;
    for (int i = 0; i < 6; ++i) {
        frame[i]     = next_hop.oct[i];
        frame[6 + i] = mac_.oct[i];
    }
    frame[12] = static_cast<uint8_t>(ethertype >> 8);  // hi byte first (wire order)
    frame[13] = static_cast<uint8_t>(ethertype & 0xFF);
    if (len > 0) {
        memcpy(frame + 14, l3, len);
    }
    const uint32_t total  = net::kHdrBytes + 14 + len;
    // TX: one device-read chain (virtio-net TX has no status byte, unlike blk).
    SgEntry        out_sg = {tx_buf_.phys(), total};
    auto           sr     = tx_queue_.submit_chain(&out_sg, 1, nullptr, 0);
    if (!sr.ok()) {
        return sr.error();
    }
    tx_queue_.kick();
    return tx_queue_.wait_completion(sr.value());
}

namespace {
VirtIONetDevice* g_virtio_net = nullptr;  ///< set from main.cpp Step 21a3
}  // namespace

VirtIONetDevice* virtio_net_device() {
    return g_virtio_net;
}
void set_virtio_net_device(VirtIONetDevice* dev) {
    g_virtio_net = dev;
}

}  // namespace cinux::drivers::virtio

// ============================================================
// MSI-X interrupt handler (F5-M2 batch 5, vector 0x43 kVirtioNetIrqVector)
// ============================================================
// Single-vector mode: RX/TX completions both raise entry 0 -> 0x43.  Counts
// only (the polling path in poll_rx/send_l3 independently observes the rings).
// NO schedule() in ISR (sti-in-syscall #DF).  EOI by the ISR_IRQ asm stub.
// Production only (test kernel has no switch_to_apic -- see virtio_blk handler).
namespace cinux::arch {
struct InterruptFrame;
}

volatile uint64_t g_virtio_net_irq_count = 0;
extern "C" void   virtio_net_irq_handler(cinux::arch::InterruptFrame* /*frame*/) {
    g_virtio_net_irq_count = g_virtio_net_irq_count + 1;
}
