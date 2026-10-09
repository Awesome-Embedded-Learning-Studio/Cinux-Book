#include "kernel/driver/pci/pci.hpp"

#include <stdint.h>

#include <optional>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/base/io.hpp"
#include "kernel/driver/pci/pci_layout.hpp"

namespace cinux::driver {

namespace {

/// @brief Configuration mechanism #1: the port that selects a dword.
constexpr uint16_t kConfigAddressPort = 0xCF8;

/// @brief Configuration mechanism #1: the port the selected dword answers on.
constexpr uint16_t kConfigDataPort = 0xCFC;

/// @brief Register offset: vendor id and device id share this dword.
constexpr uint8_t kRegIdentity = 0x00;

/// @brief Register offset: command bits and status bits.
constexpr uint8_t kRegCommand = 0x04;

/// @brief Register offset: revision, prog if, subclass and class base share this dword.
constexpr uint8_t kRegClassification = 0x08;

/// @brief Register offset: header type lives in byte 2 of this dword.
constexpr uint8_t kRegLayout = 0x0C;

/// @brief Register offset: the first of six 32-bit base address registers.
constexpr uint8_t kRegBarFirst = 0x10;

/// @brief Register offset: interrupt line and interrupt pin share this dword.
constexpr uint8_t kRegRouting = 0x3C;

/// @brief Vendor id field of the identity dword.
constexpr cinux::base::bit::BitRange kIdentityVendor{.low = 0, .width = 16};

/// @brief Device id field of the identity dword.
constexpr cinux::base::bit::BitRange kIdentityDevice{.low = 16, .width = 16};

/// @brief Revision field of the classification dword.
constexpr cinux::base::bit::BitRange kClassRevision{.low = 0, .width = 8};

/// @brief Programming interface field of the classification dword.
constexpr cinux::base::bit::BitRange kClassProgIf{.low = 8, .width = 8};

/// @brief Subclass field of the classification dword.
constexpr cinux::base::bit::BitRange kClassSubclass{.low = 16, .width = 8};

/// @brief Class-base field of the classification dword.
constexpr cinux::base::bit::BitRange kClassBase{.low = 24, .width = 8};

/// @brief Header type field of the layout dword.
constexpr cinux::base::bit::BitRange kLayoutHeaderType{.low = 16, .width = 8};

/// @brief Interrupt line field of the routing dword.
constexpr cinux::base::bit::BitRange kRoutingLine{.low = 0, .width = 8};

/// @brief Interrupt pin field of the routing dword.
constexpr cinux::base::bit::BitRange kRoutingPin{.low = 8, .width = 8};

/// @brief Command bit: the device may master the bus, which DMA needs.
constexpr cinux::base::bit::BitMask<uint32_t> kCommandBusMaster =
    cinux::base::bit::MaskBit<uint32_t>(1);

/// @brief Command bit: the device decodes its memory windows.
constexpr cinux::base::bit::BitMask<uint32_t> kCommandMemorySpace =
    cinux::base::bit::MaskBit<uint32_t>(2);

/// @brief Command bit: one means the device asserts no interrupts.
constexpr cinux::base::bit::BitMask<uint32_t> kCommandIntDisable =
    cinux::base::bit::MaskBit<uint32_t>(10);

constexpr uint8_t  kScanBuses     = 32;
constexpr uint8_t  kScanSlots     = 32;
constexpr uint8_t  kScanFunctions = 8;
constexpr uint16_t kVendorAbsent  = 0xFFFF;

using ConfigWord = cinux::base::bit::BitMask<uint32_t>;

/// @brief One configuration-space location, ready to select.
struct ConfigSpot {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t offset;

    /// @brief The same device, at another register.
    [[nodiscard]] constexpr ConfigSpot reg(uint8_t where) const {
        return {.bus = bus, .slot = slot, .func = func, .offset = where};
    }
};

void select_config(ConfigSpot spot) {
    Out32(PortWrite32{.port  = kConfigAddressPort,
                      .value = MakeConfigAddress(spot.bus, spot.slot, spot.func, spot.offset)});
}

uint32_t read_config(ConfigSpot spot) {
    select_config(spot);
    return In32(kConfigDataPort);
}

void write_config(ConfigSpot spot, uint32_t value) {
    select_config(spot);
    Out32(PortWrite32{.port = kConfigDataPort, .value = value});
}

const char* class_name(uint8_t class_code) {
    switch (class_code) {
    case 0x00:
        return "unclassified";
    case 0x01:
        return "mass storage";
    case 0x02:
        return "network";
    case 0x03:
        return "display";
    case 0x04:
        return "multimedia";
    case 0x05:
        return "memory";
    case 0x06:
        return "bridge";
    case 0x07:
        return "communication";
    case 0x08:
        return "system";
    case 0x09:
        return "input";
    case 0x0A:
        return "docking";
    case 0x0B:
        return "processor";
    case 0x0C:
        return "serial bus";
    default:
        return "reserved";
    }
}

std::optional<PciDevice> probe_location(uint8_t bus, uint8_t slot, uint8_t func) {
    const ConfigSpot kDevice{.bus = bus, .slot = slot, .func = func, .offset = 0};
    const ConfigWord kIdentity{read_config(kDevice.reg(kRegIdentity))};
    if (kIdentity.extract(kIdentityVendor) == kVendorAbsent) {
        return std::nullopt;
    }
    const ConfigWord kClassification{read_config(kDevice.reg(kRegClassification))};
    const ConfigWord kLayout{read_config(kDevice.reg(kRegLayout))};
    const ConfigWord kRouting{read_config(kDevice.reg(kRegRouting))};

    PciDevice device{};
    device.bus            = bus;
    device.slot           = slot;
    device.func           = func;
    device.vendor_id      = static_cast<uint16_t>(kIdentity.extract(kIdentityVendor));
    device.device_id      = static_cast<uint16_t>(kIdentity.extract(kIdentityDevice));
    device.revision       = static_cast<uint8_t>(kClassification.extract(kClassRevision));
    device.prog_if        = static_cast<uint8_t>(kClassification.extract(kClassProgIf));
    device.subclass       = static_cast<uint8_t>(kClassification.extract(kClassSubclass));
    device.class_code     = static_cast<uint8_t>(kClassification.extract(kClassBase));
    device.header_type    = static_cast<uint8_t>(kLayout.extract(kLayoutHeaderType));
    device.interrupt_line = static_cast<uint8_t>(kRouting.extract(kRoutingLine));
    device.interrupt_pin  = static_cast<uint8_t>(kRouting.extract(kRoutingPin));
    for (unsigned long bar = 0; bar < 6; ++bar) {
        const auto kOffset = static_cast<uint8_t>(kRegBarFirst + (bar * 4));
        device.bars[bar]   = read_config(kDevice.reg(kOffset));
    }
    return device;
}

}  // namespace

std::optional<unsigned long> PciBus::scan_slot(uint8_t bus, uint8_t slot) {
    if (!probe_location(bus, slot, 0)) {
        return 0;
    }
    unsigned long found = 0;
    for (uint8_t func = 0; func < kScanFunctions; ++func) {
        std::optional<PciDevice> device = probe_location(bus, slot, func);
        if (device) {
            if (!admit(*device)) {
                return std::nullopt;
            }
            ++found;
        }
    }
    return found;
}

void PciBus::init() {
    unsigned long found = 0;
    for (uint8_t bus = 0; bus < kScanBuses; ++bus) {
        for (uint8_t slot = 0; slot < kScanSlots; ++slot) {
            const std::optional<unsigned long> kAdmitted = scan_slot(bus, slot);
            if (!kAdmitted) {
                return;
            }
            found += *kAdmitted;
        }
    }
    print::Println("[kern] pci walk found %u devices", found);
}

bool PciBus::admit(const PciDevice& device) {
    const unsigned long kIndex = devices_.insert(device);
    if (kIndex == kDeviceCapacity) {
        print::Println("[kern] pci device table full at %u, walk stops", kDeviceCapacity);
        return false;
    }
    print::Println("[kern] pci %u:%u:%u %X:%X %s %X:%X", device.bus, device.slot, device.func,
                   device.vendor_id, device.device_id, class_name(device.class_code),
                   device.class_code, device.subclass);
    return true;
}

const PciDevice* PciBus::find_by_class(PciClassCode base, uint8_t subclass) const {
    const auto          kWantBase = static_cast<uint8_t>(base);
    const unsigned long kIndex = devices_.find_if([kWantBase, subclass](const PciDevice& device) {
        return device.class_code == kWantBase && device.subclass == subclass;
    });
    return devices_.get(kIndex);
}

void PciBus::enable_device(const PciDevice& device) {
    const ConfigSpot kSpot{
        .bus = device.bus, .slot = device.slot, .func = device.func, .offset = kRegCommand};
    ConfigWord command{read_config(kSpot)};
    command = (command | kCommandBusMaster | kCommandMemorySpace) & ~kCommandIntDisable;
    write_config(kSpot, command.raw);
}

}  // namespace cinux::driver
