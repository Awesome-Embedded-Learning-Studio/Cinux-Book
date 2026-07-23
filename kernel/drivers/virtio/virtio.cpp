/**
 * @file kernel/drivers/virtio/virtio.cpp
 * @brief VirtIODevice -- VirtIO PCI modern transport implementation (F5-M2 batch 1)
 *
 * PCI capability-list walk + BAR MMIO mapping + 64-bit feature negotiation +
 * queue register programming.  Modelled on NvmeController::init (PCI COMMAND
 * enable -> map BAR -> read registers), but the VirtIO register blocks are
 * discovered via the capability list (cap_id=0x09) rather than at fixed BAR
 * offsets, and feature negotiation is a 64-bit two-word AND with the device.
 */

#include "kernel/drivers/virtio/virtio.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/pci/pci_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::virtio {

namespace {

// MMIO sub-allocation for the VirtIO modern-transport BAR window.  Each BAR
// the caps point at is mapped 1 page here, at 0x80000 + bar_index*0x1000.
// Clear of NVMe BAR0 @+0x70000 (..+0x75FFF); VirtIO MSI-X (batch 3) will sit
// at +0x84000+, so the 6-BAR slots here (+0x80000..+0x85FFF) are reserved for
// transport BARs only (QEMU virtio-pci modern uses a single BAR anyway).
constexpr uint64_t kVirtioMmioBase = cinux::arch::KMEM_MMIO_BASE + 0x80000;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;

// common_cfg register offsets (VirtIO 1.1 virtio_pci_common_cfg le layout).
namespace CfgOff {
constexpr uint32_t DEVICE_FEATURE_SELECT = 0x00;  ///< RW selects which 32-bit feature word
constexpr uint32_t DEVICE_FEATURE        = 0x04;  ///< R  device features for the selected word
constexpr uint32_t DRIVER_FEATURE_SELECT = 0x08;  ///< RW
constexpr uint32_t DRIVER_FEATURE        = 0x0c;  ///< RW driver-acknowledged features
constexpr uint32_t NUM_QUEUES            = 0x12;  ///< R  total queues supported
constexpr uint32_t DEVICE_STATUS         = 0x14;  ///< RW status bits
constexpr uint32_t QUEUE_SELECT          = 0x16;  ///< RW selects queue for the below
constexpr uint32_t QUEUE_SIZE            = 0x18;  ///< RW queue size (power of two)
constexpr uint32_t QUEUE_MSIX_VECTOR     = 0x1a;  ///< RW MSI-X entry (0xFFFF=none)
constexpr uint32_t QUEUE_ENABLE          = 0x1c;  ///< RW 1 = enable selected queue
constexpr uint32_t QUEUE_NOTIFY_OFF      = 0x1e;  ///< R  notify-offset for this queue
constexpr uint32_t QUEUE_DESC_LO         = 0x20;  ///< RW desc-table phys (split lo/hi)
constexpr uint32_t QUEUE_DESC_HI         = 0x24;
constexpr uint32_t QUEUE_AVAIL_LO        = 0x28;  ///< RW avail-ring phys
constexpr uint32_t QUEUE_AVAIL_HI        = 0x2c;
constexpr uint32_t QUEUE_USED_LO         = 0x30;  ///< RW used-ring phys
constexpr uint32_t QUEUE_USED_HI         = 0x34;
}  // namespace CfgOff

constexpr uint32_t kCapListMax = 64;      ///< cap-list walk safety bound (anti loop)
constexpr uint32_t kResetIters = 100000;  ///< reset spin budget (no IRQ in batch 1)

}  // namespace

// ============================================================
// common_cfg register access (volatile MMIO at common_cfg_ + off)
// ============================================================

uint32_t VirtIODevice::common_read32(uint32_t off) const {
    return *reinterpret_cast<volatile uint32_t*>(common_cfg_ + off);
}
void VirtIODevice::common_write32(uint32_t off, uint32_t v) {
    *reinterpret_cast<volatile uint32_t*>(common_cfg_ + off) = v;
}
uint16_t VirtIODevice::common_read16(uint32_t off) const {
    return *reinterpret_cast<volatile uint16_t*>(common_cfg_ + off);
}
void VirtIODevice::common_write16(uint32_t off, uint16_t v) {
    *reinterpret_cast<volatile uint16_t*>(common_cfg_ + off) = v;
}
uint8_t VirtIODevice::common_read8(uint32_t off) const {
    return common_cfg_[off];
}
void VirtIODevice::common_write8(uint32_t off, uint8_t v) {
    common_cfg_[off] = v;
}

// ============================================================
// BAR mapping
// ============================================================

volatile uint8_t* VirtIODevice::map_bar(uint8_t bar_index) {
    if (bar_virt_[bar_index] != 0) {
        return reinterpret_cast<volatile uint8_t*>(bar_virt_[bar_index]);
    }
    const uint64_t phys = dev_.bar[bar_index];
    if (phys == 0) {
        cinux::lib::kprintf("[VirtIO] BAR%u unassigned (firmware did not allocate)\n", bar_index);
        return nullptr;
    }
    // Map 4 pages (16 KiB) -- the VirtIO modern register window spans
    // common(+0)/isr(+0x1000)/device(+0x2000)/notify(+0x3000) in one BAR.
    constexpr uint64_t kBarPages     = 4;
    constexpr uint64_t kBarSlot      = kBarPages * 0x1000;  // 0x4000 per BAR slot
    // Per-device stride (0x10000 = 64 KiB headroom for the 6 BAR slots) so blk
    // and net don't both land at +0x90000 for BAR4.
    constexpr uint64_t kDeviceStride = 0x10000;
    const uint64_t virt = kVirtioMmioBase + static_cast<uint64_t>(device_slot_) * kDeviceStride +
                          static_cast<uint64_t>(bar_index) * kBarSlot;
    for (uint64_t i = 0; i < kBarPages; ++i) {
        if (!cinux::mm::g_vmm.map(virt + i * 0x1000, phys + i * 0x1000, kMmioFlags)) {
            cinux::lib::kprintf("[VirtIO] BAR%u map failed at page %u (phys=0x%lx)\n", bar_index,
                                static_cast<unsigned>(i), static_cast<unsigned long>(phys));
            return nullptr;
        }
    }
    bar_virt_[bar_index] = virt;
    return reinterpret_cast<volatile uint8_t*>(virt);
}

void VirtIODevice::self_assign_bar(uint8_t bar_index) {
    const uint8_t  off   = pci::PciReg::BAR0 + static_cast<uint8_t>(bar_index * 4);
    const uint32_t orig  = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, off);
    const bool     is_64 = (orig & pci::BAR_TYPE_MASK) == pci::BAR_TYPE_64;

    // Probe the size: write all-1s (both halves if 64-bit), read back the
    // size-encoded bits in the low word.
    pci::PCI::pci_write(dev_.bus, dev_.slot, dev_.func, off, 0xFFFFFFFFu);
    if (is_64) {
        pci::PCI::pci_write(dev_.bus, dev_.slot, dev_.func, off + 4, 0xFFFFFFFFu);
    }
    const uint32_t probe = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, off);
    const uint32_t size  = ~(probe & pci::BAR_ADDR_MASK_32) + 1;

    // Assign a per-device 32-bit MMIO slot, clear of NVMe BAR0 (0xfeb40000) and
    // the AHCI BAR5 window (0xfebf1000).  device_slot_ gives each instance a
    // distinct 64 KiB region (blk -> 0xfeb60000, net -> 0xfeb70000) so the two
    // modern BAR4s no longer collide.  64-bit BARs get the upper word zeroed so
    // the address stays < 4 GB (the QEMU register window is tiny anyway).
    constexpr uint32_t kBase     = 0xfeb60000;
    const uint32_t     kAssigned = kBase + static_cast<uint32_t>(device_slot_) * 0x10000;
    pci::PCI::pci_write(dev_.bus, dev_.slot, dev_.func, off, kAssigned);
    if (is_64) {
        pci::PCI::pci_write(dev_.bus, dev_.slot, dev_.func, off + 4, 0);
    }
    dev_.bar[bar_index] = kAssigned;
    cinux::lib::kprintf("[VirtIO] BAR%u self-assigned 0x%x (slot=%u, size=%u, 64-bit=%c)\n",
                        bar_index, kAssigned, device_slot_, static_cast<unsigned>(size),
                        is_64 ? 'Y' : 'N');
}

bool VirtIODevice::resolve_cfg_pointers() {
    common_cfg_ = map_bar(caps_.common_bar);
    if (common_cfg_ == nullptr) {
        return false;
    }
    common_cfg_ += caps_.common_off;
    if (caps_.found_notify) {
        if (volatile uint8_t* b = map_bar(caps_.notify_bar)) {
            notify_base_ = b + caps_.notify_off;
        }
    }
    if (caps_.found_isr) {
        if (volatile uint8_t* b = map_bar(caps_.isr_bar)) {
            isr_base_ = b + caps_.isr_off;
        }
    }
    if (caps_.found_device) {
        if (volatile uint8_t* b = map_bar(caps_.device_bar)) {
            device_cfg_ = b + caps_.device_off;
        }
    }
    return true;
}

// ============================================================
// Capability list walk
// ============================================================

void VirtIODevice::parse_capabilities() {
    uint8_t next = static_cast<uint8_t>(
        pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, pci::PciReg::CAPABILITIES_POINTER));
    for (uint32_t guard = 0; next != 0 && guard < kCapListMax; ++guard) {
        // virtio_pci_cap layout (VirtIO 1.1 sec 5.2):
        //   b0 cap_vndr(0x09) | b1 cap_next | b2 cap_len | b3 cfg_type
        //   b4 bar | b5..7 padding
        //   b8..11 offset (le32) | b12..15 length (le32)
        //   b16..19 notify_off_multiplier (le32, NOTIFY only; cap_len >= 20)
        const uint32_t w0       = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, next);
        const uint8_t  cap_id   = static_cast<uint8_t>(w0 & 0xFF);
        const uint8_t  cap_next = static_cast<uint8_t>((w0 >> 8) & 0xFF);
        const uint8_t  cap_len  = static_cast<uint8_t>((w0 >> 16) & 0xFF);
        if (cap_id == pci::PciCapId::VIRTIO) {
            const uint8_t  cfg_type = static_cast<uint8_t>((w0 >> 24) & 0xFF);
            const uint32_t w4       = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, next + 4);
            const uint8_t  bar      = static_cast<uint8_t>(w4 & 0xFF);
            const uint32_t offset   = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, next + 8);
            const uint32_t length   = pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, next + 12);
            switch (cfg_type) {
            case CfgType::COMMON:
                caps_.found_common = true;
                caps_.common_bar   = bar;
                caps_.common_off   = offset;
                caps_.common_len   = length;
                break;
            case CfgType::NOTIFY:
                caps_.found_notify = true;
                caps_.notify_bar   = bar;
                caps_.notify_off   = offset;
                caps_.notify_len   = length;
                if (cap_len >= 20) {
                    caps_.notify_off_multiplier =
                        pci::PCI::pci_read(dev_.bus, dev_.slot, dev_.func, next + 16);
                }
                break;
            case CfgType::ISR:
                caps_.found_isr = true;
                caps_.isr_bar   = bar;
                caps_.isr_off   = offset;
                caps_.isr_len   = length;
                break;
            case CfgType::DEVICE:
                caps_.found_device = true;
                caps_.device_bar   = bar;
                caps_.device_off   = offset;
                caps_.device_len   = length;
                break;
            default:
                break;  // SHARED_MEM / VENDOR -- unused
            }
        }
        next = cap_next;
    }
}

// ============================================================
// init / reset / status
// ============================================================

cinux::lib::ErrorOr<void> VirtIODevice::init(const pci::PCIDevice& dev) {
    dev_ = dev;

    // Assign this instance a distinct MMIO slot (see device_slot_ doc) -- boot
    // is single-threaded, so a function-local counter is enough.  blk (first
    // init) -> slot 0, net (second) -> slot 1; each gets a unique BAR phys/virt
    // + MSI-X Table/PBA virt so the two no longer clobber each other.
    static unsigned next_device_slot = 0;
    device_slot_                     = next_device_slot++;

    // Enable PCI Bus Master + Memory Space so the device may master DMA and
    // expose its MMIO window.  (QEMU firmware assigns the BAR; no self-assign
    // needed, unlike NVMe whose BAR SeaBIOS skips.)
    const uint32_t cmd = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND,
                        cmd | pci::PciCmd::BUS_MASTER | pci::PciCmd::MEM_SPACE);

    parse_capabilities();
    if (!caps_.found_common) {
        cinux::lib::kprintf("[VirtIO] no common_cfg capability -- legacy-only device?\n");
        return cinux::lib::Error::IOError;
    }
    // Self-assign the modern transport BAR before mapping: SeaBIOS leaves the
    // QEMU virtio-pci modern BAR with a bogus upper word, so map_bar would
    // otherwise target RAM and read poison.  Mirrors NVMe BAR0 self-assign.
    self_assign_bar(caps_.common_bar);
    if (!resolve_cfg_pointers()) {
        return cinux::lib::Error::IOError;
    }

    // Reset, then signal ACKNOWLEDGE | DRIVER.
    auto r = reset();
    if (!r.ok()) {
        return r.error();
    }
    set_status(Status::ACKNOWLEDGE | Status::DRIVER);

    const uint16_t num_queues = common_read16(CfgOff::NUM_QUEUES);
    const uint64_t feats      = device_features();
    cinux::lib::kprintf(
        "[VirtIO] transport ready: common BAR%u+0x%x notify BAR%u+0x%x (mult=%u) "
        "isr=%c device=%c num_queues=%u features=0x%llx\n",
        caps_.common_bar, caps_.common_off, caps_.notify_bar, caps_.notify_off,
        caps_.notify_off_multiplier, caps_.found_isr ? 'Y' : 'N', caps_.found_device ? 'Y' : 'N',
        num_queues, static_cast<unsigned long long>(feats));
    return {};
}

cinux::lib::ErrorOr<void> VirtIODevice::init_msi_x(uint8_t vector) {
    msix_cap_ = pci::msix::find_capability(dev_.bus, dev_.slot, dev_.func, &pci::PCI::pci_read);
    if (!msix_cap_.found) {
        cinux::lib::kprintf("[VirtIO] no MSI-X capability\n");
        return cinux::lib::Error::InvalidArgument;
    }
    // Per-device MSI-X Table/PBA virt: blk (slot 0) -> +0x84000/+0x85000, net
    // (slot 1) -> +0x86000/+0x87000 (xHCI owns +0x40000, NVMe +0x74000).  Each
    // device gets a distinct 0x2000 slot so the two Tables no longer collide at
    // +0x84000 (the plan's intent, now actually implemented).  Entry 0 ->
    // @p vector; program_vector unmasks.  LEAVE entry 0 unmasked -- real async
    // IRQ.  Production only (test kernel has no switch_to_apic -> MSI would
    // strand in the LAPIC ISR, NVMe batch-3 root cause).
    const uint64_t kTableVirt =
        cinux::arch::KMEM_MMIO_BASE + 0x84000 + static_cast<uint64_t>(device_slot_) * 0x2000;
    const uint64_t kPbaVirt = kTableVirt + 0x1000;
    auto           r        = msix_.init(msix_cap_, dev_, kTableVirt, kPbaVirt);
    if (!r.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    msix_.mask_all();
    msix_.program_vector(0, vector, 0);
    msix_.enable();
    // NOT mask_all() here -- entry 0 stays unmasked so the device's completion
    // MSI fires for real.  Production arms the IDT handler (irq_init) + has
    // switch_to_apic (Step 17), so the MSI delivers instead of stranding.
    cinux::lib::kprintf("[VirtIO] MSI-X enabled (entry 0 -> vector 0x%x, %u entries, unmasked)\n",
                        vector, msix_cap_.table_size);
    return {};
}

cinux::lib::ErrorOr<void> VirtIODevice::reset() {
    common_write8(CfgOff::DEVICE_STATUS, 0);
    for (uint32_t i = 0; i < kResetIters; ++i) {
        if (common_read8(CfgOff::DEVICE_STATUS) == 0) {
            return {};
        }
    }
    cinux::lib::kprintf("[VirtIO] reset timeout (status=0x%x)\n",
                        common_read8(CfgOff::DEVICE_STATUS));
    return cinux::lib::Error::TimedOut;
}

uint64_t VirtIODevice::device_features() {
    uint64_t features = 0;
    for (uint32_t word = 0; word < 2; ++word) {
        common_write32(CfgOff::DEVICE_FEATURE_SELECT, word);
        features |= static_cast<uint64_t>(common_read32(CfgOff::DEVICE_FEATURE)) << (word * 32);
    }
    return features;
}

cinux::lib::ErrorOr<uint64_t> VirtIODevice::negotiate_features(uint64_t wanted) {
    uint64_t negotiated = 0;
    for (uint32_t word = 0; word < 2; ++word) {
        common_write32(CfgOff::DEVICE_FEATURE_SELECT, word);
        const uint32_t dev_word = common_read32(CfgOff::DEVICE_FEATURE);
        common_write32(CfgOff::DRIVER_FEATURE_SELECT, word);
        const uint32_t want_word = static_cast<uint32_t>((wanted >> (word * 32)) & 0xFFFFFFFFu);
        const uint32_t neg_word  = want_word & dev_word;
        common_write32(CfgOff::DRIVER_FEATURE, neg_word);
        negotiated |= static_cast<uint64_t>(neg_word) << (word * 32);
    }
    set_status(Status::FEATURES_OK);
    if ((status() & Status::FEATURES_OK) == 0) {
        cinux::lib::kprintf("[VirtIO] FEATURES_OK rejected (wanted=0x%llx dev=0x%llx)\n",
                            static_cast<unsigned long long>(wanted),
                            static_cast<unsigned long long>(device_features()));
        return cinux::lib::Error::IOError;
    }
    negotiated_ = negotiated;
    return negotiated;
}

uint8_t VirtIODevice::status() const {
    return common_read8(CfgOff::DEVICE_STATUS);
}
void VirtIODevice::set_status(uint8_t bits) {
    common_write8(CfgOff::DEVICE_STATUS, static_cast<uint8_t>(status() | bits));
}
void VirtIODevice::clear_status(uint8_t bits) {
    common_write8(CfgOff::DEVICE_STATUS,
                  static_cast<uint8_t>(status() & static_cast<uint8_t>(~bits)));
}

// ============================================================
// queue configuration
// ============================================================

cinux::lib::ErrorOr<uint16_t> VirtIODevice::setup_queue(uint16_t queue_index, uint16_t size,
                                                        uint64_t desc_phys, uint64_t avail_phys,
                                                        uint64_t used_phys) {
    common_write16(CfgOff::QUEUE_SELECT, queue_index);
    common_write16(CfgOff::QUEUE_SIZE, size);
    // Bind this queue to MSI-X entry 0 so completions raise vector 0x42 once
    // init_msi_x enables MSI-X.  No effect when MSI-X is off (test kernel).
    common_write16(CfgOff::QUEUE_MSIX_VECTOR, 0);
    // 64-bit ring phys written as two 32-bit halves (avoids 64-bit MMIO
    // alignment concerns; VirtIO common_cfg queue_{desc,avail,used} are le64).
    common_write32(CfgOff::QUEUE_DESC_LO, static_cast<uint32_t>(desc_phys));
    common_write32(CfgOff::QUEUE_DESC_HI, static_cast<uint32_t>(desc_phys >> 32));
    common_write32(CfgOff::QUEUE_AVAIL_LO, static_cast<uint32_t>(avail_phys));
    common_write32(CfgOff::QUEUE_AVAIL_HI, static_cast<uint32_t>(avail_phys >> 32));
    common_write32(CfgOff::QUEUE_USED_LO, static_cast<uint32_t>(used_phys));
    common_write32(CfgOff::QUEUE_USED_HI, static_cast<uint32_t>(used_phys >> 32));
    return common_read16(CfgOff::QUEUE_NOTIFY_OFF);
}

void VirtIODevice::enable_queue(uint16_t queue_index) {
    common_write16(CfgOff::QUEUE_SELECT, queue_index);
    common_write16(CfgOff::QUEUE_ENABLE, 1);
}

void VirtIODevice::notify_queue(uint16_t queue_index, uint16_t notify_off) const {
    if (notify_base_ == nullptr) {
        return;
    }
    volatile uint8_t* mmio =
        notify_base_ + static_cast<uint32_t>(notify_off) * caps_.notify_off_multiplier;
    *reinterpret_cast<volatile uint16_t*>(mmio) = queue_index;
}

uint8_t VirtIODevice::read_isr() const {
    return isr_base_ ? isr_base_[0] : 0;
}

// ============================================================
// device-specific config (device_cfg)
// ============================================================

uint8_t VirtIODevice::device_cfg_read8(uint32_t off) const {
    return device_cfg_ ? device_cfg_[off] : 0;
}
uint32_t VirtIODevice::device_cfg_read32(uint32_t off) const {
    return device_cfg_ ? *reinterpret_cast<volatile uint32_t*>(device_cfg_ + off) : 0;
}
uint64_t VirtIODevice::device_cfg_read64(uint32_t off) const {
    if (!device_cfg_) {
        return 0;
    }
    // Two 32-bit halves -- avoids 64-bit MMIO alignment concerns.
    const uint32_t lo = *reinterpret_cast<volatile uint32_t*>(device_cfg_ + off);
    const uint32_t hi = *reinterpret_cast<volatile uint32_t*>(device_cfg_ + off + 4);
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

}  // namespace cinux::drivers::virtio
