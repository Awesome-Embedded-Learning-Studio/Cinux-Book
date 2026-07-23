/**
 * @file kernel/drivers/virtio/virtio.hpp
 * @brief VirtIODevice -- VirtIO PCI modern transport (F5-M2 batch 1)
 *
 * Discovers a VirtIO PCI device (vendor 0x1AF4), walks the PCI capability
 * list to locate the modern transport register blocks (common_cfg / notify /
 * isr / device_cfg), maps the named BAR into the MMIO window, and drives the
 * VirtIO status machine + 64-bit feature negotiation.  virtqueue (split queue)
 * management lives in virtqueue.hpp.
 *
 * Modern transport only (VirtIO 1.0+, capability-based).  Legacy transport
 * (port I/O at BAR0) is NOT supported -- QEMU's -device virtio-*-pci defaults
 * to transitional (PCI device_id reads as the legacy value) but advertises the
 * modern capability list, which we drive exclusively.
 *
 * Modelled on NvmeController (instance + init(const PCIDevice&)).  NOT
 * all-static: VirtIO carries per-device mutable state (negotiated features,
 * capability locations, BAR mappings).  MSI-X is deferred to batch 3.
 *
 * Batches:
 *   1 -- PCI find + BAR map + cap parse + feature negotiation + status machine
 *        + virtqueue split queue + polling round-trip mechanism test.
 *   2 -- virtio-blk device driver (read/write/flush + IBlockDevice adapter).
 *   3 -- MSI-X real interrupt + SMP (Spinlock-protected shared state).
 *
 * Namespace: cinux::drivers::virtio
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/pci/msix.hpp"
#include "kernel/drivers/pci/pci.hpp"

namespace cinux::drivers::virtio {

/// Fixed IDT vector for the VirtIO-blk MSI-X interrupt (entry 0). Clear of
/// NVMe (0x41) / xHCI (0x40) / LAPIC timer (0x30). Registered in irq_init;
/// fires once init_msi_x enables + unmasks MSI-X.
constexpr uint8_t kVirtioBlkIrqVector = 0x42;
/// VirtIO-net MSI-X vector (batch 5).  Single-vector mode: RX/TX share entry 0
/// (multi-vector 0x43-0x45 is a follow-up; the single path proves the real
/// async IRQ seam).  Clear of blk (0x42) / NVMe (0x41) / xHCI (0x40).
constexpr uint8_t kVirtioNetIrqVector = 0x43;

// ============================================================
// VirtIO device_status bits (common_cfg + 0x14)
// ============================================================
namespace Status {
constexpr uint8_t ACKNOWLEDGE       = 0x01;  ///< OS noticed the device
constexpr uint8_t DRIVER            = 0x02;  ///< OS knows how to drive it
constexpr uint8_t DRIVER_OK         = 0x04;  ///< driver set up; device may init
constexpr uint8_t FEATURES_OK       = 0x08;  ///< feature negotiation complete
constexpr uint8_t DEVICE_NEED_RESET = 0x40;  ///< device unrecoverable error
constexpr uint8_t FAILED            = 0x80;  ///< driver gave up
}  // namespace Status

// ============================================================
// VirtIO PCI capability cfg_type values (VirtIO 1.1 sec 5.2)
// ============================================================
namespace CfgType {
constexpr uint8_t COMMON     = 1;  ///< common_cfg (queue + feature registers)
constexpr uint8_t NOTIFY     = 2;  ///< notify (kick doorbell per queue)
constexpr uint8_t ISR        = 3;  ///< ISR status (read-to-clear)
constexpr uint8_t DEVICE     = 4;  ///< device-specific config (e.g. blk capacity)
constexpr uint8_t SHARED_MEM = 5;
constexpr uint8_t VENDOR     = 6;
}  // namespace CfgType

// ============================================================
// VirtIO feature bits (common across device types)
// ============================================================
// Bits 0..23 are device-specific (defined in virtio_blk.hpp / virtio_net.hpp).
// Bits 24+ are transport-independent.
namespace Feature {
constexpr uint64_t RING_INDIRECT_DESC = 1ULL << 28;  ///< chained indirect desc
constexpr uint64_t RING_EVENT_IDX     = 1ULL << 29;  ///< used/avail event idx
constexpr uint64_t VERSION_1          = 1ULL << 32;  ///< VirtIO 1.0 (modern) device
constexpr uint64_t ACCESS_PLATFORM    = 1ULL << 33;
constexpr uint64_t RING_PACKED        = 1ULL << 34;  ///< packed virtqueue (unused)
constexpr uint64_t IN_ORDER           = 1ULL << 35;
}  // namespace Feature

/// Decoded locations of the four modern-transport register blocks.  Each block
/// lives at (bar[index], offset) and is `length` bytes long.  A single BAR
/// typically hosts all four (QEMU virtio-pci modern uses one 4 KiB BAR), but
/// the spec permits them to be split across BARs, so we track per-block.
struct VirtioCapLocations {
    bool found_common = false;
    bool found_notify = false;
    bool found_isr    = false;
    bool found_device = false;

    uint8_t  common_bar = 0;
    uint32_t common_off = 0;
    uint32_t common_len = 0;
    uint8_t  notify_bar = 0;
    uint32_t notify_off = 0;
    uint32_t notify_len = 0;
    uint8_t  isr_bar    = 0;
    uint32_t isr_off    = 0;
    uint32_t isr_len    = 0;
    uint8_t  device_bar = 0;
    uint32_t device_off = 0;
    uint32_t device_len = 0;

    /// Multiplier applied to a queue's notify_off to compute the kick address:
    ///   notify_addr = notify_base + notify_off * notify_off_multiplier.
    /// Only the NOTIFY capability carries this field (its cap_len is 20, others 16).
    uint32_t notify_off_multiplier = 0;
};

/**
 * @brief VirtIO PCI modern transport controller
 *
 * Owns the PCI device descriptor, decoded capability locations, and the
 * per-BAR MMIO mappings.  All common_cfg/notify/isr/device_cfg access goes
 * through the cached virt pointers so callers work in plain offsets.
 */
class VirtIODevice {
public:
    /// Discover the VirtIO transport: enable PCI Bus Master + Memory Space,
    /// walk the capability list, map the named BAR(s), then reset +
    /// ACKNOWLEDGE | DRIVER.  Does NOT negotiate features or set DRIVER_OK --
    /// call negotiate_features() + finish_init().
    cinux::lib::ErrorOr<void> init(const pci::PCIDevice& dev);

    /// Configure MSI-X (batch 3): map Table/PBA at +0x84000/+0x85000, program
    /// entry 0 -> @p vector, enable, and LEAVE entry 0 unmasked (real async
    /// interrupt -- unlike NVMe's mask_all polling).  The caller MUST have
    /// installed the IDT handler first (irq_init registers kVirtioBlkIrqVector).
    /// Production path; the test kernel leaves MSI-X off (no switch_to_apic ->
    /// an unmasked MSI would strand in the LAPIC ISR, re-NVMe-batch-3 root cause).
    cinux::lib::ErrorOr<void> init_msi_x(uint8_t vector);

    /// Reset the device (device_status <- 0) and wait for it to settle.
    /// Required before a fresh feature negotiation.
    cinux::lib::ErrorOr<void> reset();

    /// Negotiate features: for each 32-bit feature word, write
    /// driver_features = wanted & device_features, then set FEATURES_OK and
    /// read it back (the device clears FEATURES_OK if it rejects the set).
    /// @return the negotiated feature set, or Error::IOError if rejected.
    cinux::lib::ErrorOr<uint64_t> negotiate_features(uint64_t wanted);

    /// Read the full 64-bit device feature set (both 32-bit words).
    /// Non-const: reads require writing device_feature_select first.
    uint64_t device_features();

    /// Read the device_status byte.
    uint8_t status() const;
    /// OR bits into device_status.
    void    set_status(uint8_t bits);
    /// Clear bits from device_status.
    void    clear_status(uint8_t bits);

    /// Configure a queue: select it, set queue_size, and write the desc/avail/
    /// used ring DMA addresses.  The rings must already be allocated + zeroed
    /// DMA.  @return the queue's notify_off (use notify_queue() to kick).
    /// Does NOT enable the queue -- call enable_queue() after.
    cinux::lib::ErrorOr<uint16_t> setup_queue(uint16_t queue_index, uint16_t size,
                                              uint64_t desc_phys, uint64_t avail_phys,
                                              uint64_t used_phys);

    /// Enable a configured queue (queue_select + queue_enable <- 1).
    void enable_queue(uint16_t queue_index);

    /// Kick a queue: write the queue index to notify_base + notify_off *
    /// notify_off_multiplier.  @p notify_off is the value returned by setup_queue.
    void notify_queue(uint16_t queue_index, uint16_t notify_off) const;

    /// Read the ISR status byte (reading clears it).
    /// bit 0 = queue interrupt, bit 1 = device config change.
    uint8_t read_isr() const;

    /// Read the device-specific config region (device_cfg) at byte @p off.
    uint8_t  device_cfg_read8(uint32_t off) const;
    uint32_t device_cfg_read32(uint32_t off) const;
    uint64_t device_cfg_read64(uint32_t off) const;

    bool                      present() const { return common_cfg_ != nullptr; }
    const VirtioCapLocations& caps() const { return caps_; }
    uint16_t                  negotiated() const { return static_cast<uint16_t>(negotiated_); }

private:
    /// Walk the PCI capability list (from offset 0x34) collecting VirtIO
    /// (cap_id=0x09) entries into caps_.
    void parse_capabilities();

    /// Map BAR @p bar_index into the MMIO window (4 pages) if not already mapped,
    /// and return its virt base.  Returns nullptr on map failure.
    volatile uint8_t* map_bar(uint8_t bar_index);

    /// Self-assign a fixed MMIO slot to BAR @p bar_index.  SeaBIOS does not
    /// reliably allocate the QEMU virtio-pci modern BAR (BAR4 carries a bogus
    /// 64-bit prefetchable marker with a junk upper word), so probe the size
    /// and write a fixed CinuxOS slot, mirroring the NVMe BAR0 self-assign.
    void self_assign_bar(uint8_t bar_index);

    /// Resolve the common_cfg/notify/isr/device_cfg virt pointers after caps_
    /// is parsed and the named BAR(s) are mapped.
    bool resolve_cfg_pointers();

    // 32-bit common_cfg register access (dword-aligned offsets within common_cfg_).
    uint32_t common_read32(uint32_t off) const;
    void     common_write32(uint32_t off, uint32_t v);
    uint16_t common_read16(uint32_t off) const;
    void     common_write16(uint32_t off, uint16_t v);
    uint8_t  common_read8(uint32_t off) const;
    void     common_write8(uint32_t off, uint8_t v);

    pci::PCIDevice     dev_{};
    VirtioCapLocations caps_{};
    uint64_t           negotiated_ = 0;

    /// Per-instance slot (assigned in init() from a monotonic counter) so each
    /// VirtIO device gets a DISTINCT MMIO slot for its self-assigned transport
    /// BAR phys + virt, and its MSI-X Table/PBA virt.  Without this, blk + net
    /// both self-assign the same fixed BAR4 phys (0xfeb60000) + virt (+0x90000)
    /// + MSI-X Table virt (+0x84000) -- the second device's init clobbers the
    /// first's mapping.  Slot N gets transport BAR phys 0xfeb60000+N*0x10000,
    /// virt +0x80000+N*0x10000+bar*0x4000, MSI-X Table +0x84000+N*0x2000.
    unsigned device_slot_ = 0;

    /// Per-BAR MMIO virt base (0 = unmapped).  VirtIO modern typically uses a
    /// single BAR for all of common/notify/isr/device, but we cache per-BAR so
    /// a split layout still resolves.  Mapped at KMEM_MMIO + 0x80000 + slot*0x10000 + bar*0x4000.
    static constexpr uint8_t kMaxBars            = 6;
    uint64_t                 bar_virt_[kMaxBars] = {};

    // MSI-X (batch 3).  Third MsixController instance (xHCI @+0x40000, NVMe
    // @+0x74000); VirtIO-blk Table @+0x84000, PBA @+0x85000.
    pci::msix::MsixCap        msix_cap_{};
    pci::msix::MsixController msix_;

    volatile uint8_t* common_cfg_  = nullptr;  ///< common_cfg base (virt)
    volatile uint8_t* notify_base_ = nullptr;  ///< notify region base (virt)
    volatile uint8_t* isr_base_    = nullptr;  ///< isr region base (virt)
    volatile uint8_t* device_cfg_  = nullptr;  ///< device_cfg base (virt)
};

}  // namespace cinux::drivers::virtio
