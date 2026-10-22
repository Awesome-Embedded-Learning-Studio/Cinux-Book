/**
 * @file    xhci.cpp
 * @brief   The xHCI controller layer: registers, rings, and the drain.
 *
 * Register offsets relative to each region are the only thing
 * hand-written here; region bases come from the capability block, DMA
 * pages come from the physical allocator, and the ring protocol lives
 * in xhci_layout. Sixty-four-bit registers take two thirty-two-bit
 * writes, low half first so the high write commits the complete
 * address. Transfer events route to seated devices through one erased
 * seam, command completions to the waiter that asked. Delivery arms
 * MSI-X for the day real interrupts arrive, and the drain carries the
 * cargo meanwhile — the v1 boundary kept honestly.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/usb/xhci.hpp"

#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/driver/apic/local_apic.hpp"
#include "kernel/driver/msi/msi_layout.hpp"
#include "kernel/driver/msi/msix.hpp"
#include "kernel/driver/pci/pci.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "kernel/driver/usb/usb_config.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"
#include "kernel/interrupt/irq.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::driver {

namespace {

/// @brief Operational register: run and command bits.
constexpr unsigned long kRegUsbCommand = 0x00;

/// @brief Operational register: halt and readiness bits.
constexpr unsigned long kRegUsbStatus = 0x04;

/// @brief Operational register: command ring control, sixty-four bits.
constexpr unsigned long kRegCrcr = 0x18;

/// @brief Operational register: device context array base, sixty-four bits.
constexpr unsigned long kRegDcbaap = 0x30;

/// @brief Operational register: how many slots this boot enables.
constexpr unsigned long kRegConfig = 0x38;

/// @brief Runtime register, interrupter zero: interrupt management.
constexpr unsigned long kRegIman = 0x00;

constexpr unsigned long kInterrupterZero = 0x20;

constexpr uint32_t kHostSystemError = cinux::base::bit::MaskBit<uint32_t>(2).raw;

constexpr uint64_t kEventHandlerBusy = cinux::base::bit::MaskBit<uint64_t>(3).raw;

/// @brief Runtime register, interrupter zero: segment table size.
constexpr unsigned long kRegErstsz = 0x08;

/// @brief Runtime register, interrupter zero: segment table base.
constexpr unsigned long kRegErstba = 0x10;

/// @brief Runtime register, interrupter zero: dequeue pointer.
constexpr unsigned long kRegErdp = 0x18;

/// @brief USBCMD bit: start the controller.
constexpr uint32_t kRunStop = 1U;

/// @brief USBCMD bit: reset the controller.
constexpr uint32_t kHostReset = cinux::base::bit::MaskBit<uint32_t>(1).raw;

/// @brief USBSTS bit: the controller has halted.
constexpr uint32_t kHcHalted = 1U;

/// @brief USBSTS bit: the controller is still busy with a reset.
constexpr uint32_t kNotReady = cinux::base::bit::MaskBit<uint32_t>(11).raw;

/// @brief IMAN bit: let this interrupter deliver.
constexpr uint32_t kInterruptEnable = cinux::base::bit::MaskBit<uint32_t>(1).raw;

/// @brief Where the ERST entry sits inside the event page, ring above it.
constexpr unsigned long kErstOffset = 0x800;

/// @brief One page of zeros, the honest size of a DMA page.
constexpr unsigned long kPageBytes = 4096;

/// @brief One page of the MSI-X table window, enough for this boot.
constexpr unsigned long kMsixWindowBytes = 4096;

/// @brief Bounded spins for a hypervisor that answers in microseconds.
constexpr unsigned long kSpinLimit = 2000000;

/// @brief The command ring's target: doorbell zero, stream zero.
constexpr unsigned long kCommandDoorbell = 0;

/// @brief Operational register base: the port status registers.
constexpr unsigned long kRegPortSc = 0x400;

/// @brief Port register stride, one bank per root-hub port.
constexpr unsigned long kPortStride = 0x10;

/// @brief PORTSC bit: a device is attached to this port.
constexpr uint32_t kPortConnected = cinux::base::bit::MaskBit<uint32_t>(0).raw;

/// @brief PORTSC bit: reset signaling, self-clearing.
constexpr uint32_t kPortReset = cinux::base::bit::MaskBit<uint32_t>(4).raw;

/// @brief PORTSC field: the attached device's speed code.
constexpr cinux::base::bit::BitRange kPortSpeed{.low = 10, .width = 4};

/// @brief Event control field: which endpoint a transfer event names.
constexpr cinux::base::bit::BitRange kEventEndpoint{.low = 16, .width = 5};

/// @brief The PCI shape this controller answers as.
constexpr uint8_t kPciSubclassUsbLocal = 0x03;

/// @brief The drain's message handler: one word, no context.
void drain_thunk() {
    XhciHost::self().drain_events();
}

}  // namespace

bool XhciHost::init() {
    const PciDevice* const kHost =
        PciBus::self().find_by_class(PciClassCode::kSerialBus, kPciSubclassUsbLocal);
    if (kHost == nullptr ||
        !usb::IsXhciController(kHost->class_code, kHost->subclass, kHost->prog_if)) {
        return false;
    }
    PciBus::self().enable_device(*kHost);
    const uint64_t kWindow = BarBase(kHost->bars, 0);
    if (kWindow == 0 || !mm::MapDeviceWindow(kWindow, usb::kXhciWindowBytes)) {
        return false;
    }
    window_  = reinterpret_cast<volatile unsigned char*>(  // NOLINT(performance-no-int-to-ptr)
        mm::IoremapVirt(static_cast<unsigned long>(kWindow)));
    regions_ = usb::LocateRegions(window_);
    shape_   = usb::ReadShape(window_);
    if (!halt_and_reset() || !arm_rings() || !start_controller()) {
        window_ = nullptr;
        return false;
    }
    print::Println("[kern] xhci %X ports %u slots %u running", shape_.version, shape_.max_ports,
                   shape_.max_slots);
    arm_delivery(kHost);
    return true;
}

bool XhciHost::present() const {
    return window_ != nullptr;
}

unsigned int XhciHost::max_ports() const {
    return shape_.max_ports;
}

bool XhciHost::unhealthy() const {
    return window_ != nullptr &&
           (read32(regions_.operational + kRegUsbStatus) & kHostSystemError) != 0;
}

uint32_t XhciHost::read32(unsigned long offset) const {
    return *reinterpret_cast<const volatile uint32_t*>(window_ + offset);
}

void XhciHost::write32(unsigned long offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(window_ + offset) = value;
}

void XhciHost::write64(unsigned long offset, uint64_t value) {
    write32(offset, static_cast<uint32_t>(value & 0xFFFFFFFFU));
    write32(offset + 4, static_cast<uint32_t>(value >> 32U));
}

bool XhciHost::wait_set(unsigned long offset, uint32_t mask) {
    for (unsigned long spin = 0; spin < kSpinLimit; ++spin) {
        if ((read32(offset) & mask) != 0) {
            return true;
        }
        arch::CpuRelax();
    }
    return false;
}

bool XhciHost::wait_clear(unsigned long offset, uint32_t mask) {
    for (unsigned long spin = 0; spin < kSpinLimit; ++spin) {
        if ((read32(offset) & mask) == 0) {
            return true;
        }
        arch::CpuRelax();
    }
    return false;
}

bool XhciHost::halt_and_reset() {
    write32(regions_.operational + kRegUsbCommand, 0);
    if (!wait_set(regions_.operational + kRegUsbStatus, kHcHalted)) {
        return false;
    }
    write32(regions_.operational + kRegUsbCommand, kHostReset);
    return wait_clear(regions_.operational + kRegUsbCommand, kHostReset) &&
           wait_clear(regions_.operational + kRegUsbStatus, kNotReady);
}

bool XhciHost::arm_rings() {
    const base::PhysAddr kCommand = mm::Pmm::self().allocate_page();
    const base::PhysAddr kEvent   = mm::Pmm::self().allocate_page();
    const base::PhysAddr kDevices = mm::Pmm::self().allocate_page();
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kCommand.raw)), 0, kPageBytes);
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kEvent.raw)), 0, kPageBytes);
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kDevices.raw)), 0, kPageBytes);
    usb::InitRing(base::PtrAt<usb::Trb>(mm::DirectMapVirt(kCommand.raw)), usb::kCommandRingTrbs,
                  kCommand.raw);
    auto* const kTable   = base::PtrAt<usb::ErstEntry>(mm::DirectMapVirt(kEvent.raw + kErstOffset));
    kTable->address_low  = static_cast<uint32_t>(kEvent.raw);
    kTable->address_high = static_cast<uint32_t>(kEvent.raw >> 32U);
    kTable->size         = static_cast<uint32_t>(usb::kEventRingTrbs);
    command_page_        = kCommand.raw;
    event_page_          = kEvent.raw;
    device_page_         = kDevices.raw;
    command_             = usb::RingState{.index = 0, .cycle = true};
    const unsigned int kSlots =
        shape_.max_slots < usb::kXhciSlotsEnabled ? shape_.max_slots : usb::kXhciSlotsEnabled;
    write32(regions_.operational + kRegConfig, kSlots);
    write64(regions_.operational + kRegDcbaap, device_page_);
    write32(regions_.runtime + kInterrupterZero + kRegErstsz, 1);
    write64(regions_.runtime + kInterrupterZero + kRegErstba, event_page_ + kErstOffset);
    write64(regions_.runtime + kInterrupterZero + kRegErdp, event_page_);
    write64(regions_.operational + kRegCrcr, command_page_ | 1U);
    return true;
}

bool XhciHost::start_controller() {
    write32(regions_.operational + kRegUsbCommand, kRunStop);
    return wait_clear(regions_.operational + kRegUsbStatus, kHcHalted);
}

bool XhciHost::run_command(uint64_t parameter, uint32_t control, uint8_t& slot) {
    const uint64_t kTrb = command_page_ + (command_.index * sizeof(usb::Trb));
    usb::EnqueueCommand(base::PtrAt<usb::Trb>(mm::DirectMapVirt(command_page_)),
                        usb::kCommandRingTrbs, command_, command_page_, parameter, 0, control);
    write32(regions_.doorbells + kCommandDoorbell, 0);
    uint8_t code = 0;
    slot         = 0;
    if (!await_command(kTrb, slot, code)) {
        print::Println("[kern] xhci command %X never answered", control);
        return false;
    }
    if (code != usb::kCompletionSuccess) {
        print::Println("[kern] xhci command %X refused, completion %u", control, code);
        return false;
    }
    return true;
}

void XhciHost::write_doorbell(unsigned int slot, unsigned int target) {
    write32(regions_.doorbells + (static_cast<unsigned long>(slot) * 4UL),
            static_cast<uint32_t>(target));
}

uint32_t XhciHost::port_status(unsigned int port) const {
    return read32(regions_.operational + kRegPortSc +
                  (static_cast<unsigned long>(port - 1) * kPortStride));
}

bool XhciHost::reset_port(unsigned int port, uint8_t& speed) {
    const unsigned long kPortSc =
        regions_.operational + kRegPortSc + (static_cast<unsigned long>(port - 1) * kPortStride);
    write32(kPortSc, kPortReset);
    if (!wait_clear(kPortSc, kPortReset)) {
        return false;
    }
    speed = static_cast<uint8_t>(
        cinux::base::bit::BitMask<uint32_t>{read32(kPortSc)}.extract(kPortSpeed));
    return true;
}

void XhciHost::seat_device(uint8_t slot, void* device,
                           void (*deliver)(void* device, const usb::Trb& event)) {
    if (slot >= 1 && slot <= usb::kXhciSlotsEnabled) {
        routes_[slot - 1] = usb::DeviceRoute{.device = device, .deliver = deliver};
    }
}

void XhciHost::consume_event() {
    ++event_index_;
    if (event_index_ == usb::kEventRingTrbs) {
        event_index_ = 0;
        event_cycle_ = !event_cycle_;
    }
    write64(regions_.runtime + kInterrupterZero + kRegErdp,
            (event_page_ + (event_index_ * sizeof(usb::Trb))) | kEventHandlerBusy);
}

// NOLINTNEXTLINE(readability-function-size) drain names three cargos
void XhciHost::drain_events() {
    if ((read32(regions_.operational + kRegUsbStatus) & kHostSystemError) != 0) {
        return;
    }
    const auto* const kEvents = base::PtrAt<const usb::Trb>(mm::DirectMapVirt(event_page_));
    while (true) {
        const usb::Trb& event = kEvents[event_index_];
        if (!usb::EventPending(event, event_cycle_)) {
            return;
        }
        const uint8_t kSlot = usb::SlotIdOf(event);
        const uint8_t kType = usb::TrbType(event);
        consume_event();
        if (kType == usb::kTrbTransferEvent && kSlot >= 1 && kSlot <= usb::kXhciSlotsEnabled &&
            routes_[kSlot - 1].deliver != nullptr) {
            routes_[kSlot - 1].deliver(routes_[kSlot - 1].device, event);
            continue;
        }
        if (kType == usb::kTrbCommandCompletion && event.parameter == awaited_trb_) {
            command_ready_ = true;
            command_slot_  = kSlot;
            command_code_  = usb::CompletionCode(event);
        }
    }
}

bool XhciHost::await_command(uint64_t trb_address, uint8_t& slot, uint8_t& code) {
    awaited_trb_   = trb_address;
    command_ready_ = false;
    for (unsigned long spin = 0; spin < kSpinLimit; ++spin) {
        drain_events();
        if (command_ready_) {
            slot = command_slot_;
            code = command_code_;
            return true;
        }
        if (unhealthy()) {
            return false;
        }
        arch::CpuRelax();
    }
    return false;
}

// NOLINTNEXTLINE(readability-function-size) the wiring names five hands
void XhciHost::arm_delivery(const PciDevice* host) {
    const PciSpot  kSpot{.bus = host->bus, .slot = host->slot, .func = host->func};
    msi::MsixFacts facts{};
    if (!msi::DecodeMsix(&ReadSpotConfig, nullptr, kSpot, facts)) {
        print::Println("[kern] xhci msix absent, the drain carries the cargo");
        return;
    }
    const base::Result<interrupt::MsiVector> kSeated =
        interrupt::Irq::self().seat_message(&drain_thunk);
    if (!kSeated.ok()) {
        print::Println("[kern] xhci message pool full, the drain carries the cargo");
        return;
    }
    const msi::MsixWindows kWindows = msi::ResolveMsixWindows(facts, host->bars);
    if (!mm::MapDeviceWindow(kWindows.table, kMsixWindowBytes)) {
        print::Println("[kern] xhci msix window refused, the drain carries the cargo");
        return;
    }
    auto* const kEntry = base::PtrAt<msi::MsixEntry>(mm::IoremapVirt(kWindows.table));
    if (!msi::ArmMsixEntry(*kEntry, msi::XapicMessageAddress(LocalApic::self().id()),
                           kSeated.value().value)) {
        print::Println("[kern] xhci msix entry stayed masked, the drain carries the cargo");
        return;
    }
    write32(regions_.runtime + kInterrupterZero + kRegIman, kInterruptEnable);
    print::Println("[kern] xhci msix armed, vector %u", kSeated.value().value);
}

}  // namespace cinux::driver
