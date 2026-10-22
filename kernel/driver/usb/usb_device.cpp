/**
 * @file    usb_device.cpp
 * @brief   The device layer's transfers, one class driver's type at a time.
 *
 * The endpoint-zero control dance, the interrupt ring's read, and the
 * completion dispatch live here; every register word goes through the
 * host controller's faces, every context and ring word through the
 * layout vocabulary. Explicit instantiation happens at the class
 * drivers that own devices.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.2
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/usb/usb_device.hpp"

#include <stdint.h>

#include "cinux/addr.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/driver/usb/usb_config.hpp"
#include "kernel/driver/usb/xhci.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"

namespace cinux::driver::usb {

namespace {

/// @brief Where the control data buffer sits inside the endpoint page.
constexpr unsigned long kControlBufferOffset = 0x800;

/// @brief Where the report buffer sits inside the interrupt page.
constexpr unsigned long kReportOffset = 0x800;

/// @brief How many bytes one keyboard report carries.
constexpr unsigned long kReportBytes = 8;

/// @brief One page of zeros, the honest size of a DMA page.
constexpr unsigned long kPageBytes = 4096;

/// @brief Bounded spins for a hypervisor that answers in microseconds.
constexpr unsigned long kSpinLimit = 2000000;

/// @brief The interrupt endpoint's doorbell target: endpoint zero is one.
constexpr unsigned long kInterruptEndpointId = 3;

/// @brief The control endpoint's doorbell target.
constexpr unsigned long kControlEndpointId = 1;

/// @brief Port speed code for a high-speed device.
constexpr uint8_t kSpeedHigh = 3;

/// @brief Endpoint zero's packet ceiling at high speed.
constexpr uint16_t kPacketHigh = 64;

/// @brief Endpoint zero's packet ceiling otherwise.
constexpr uint16_t kPacketElse = 8;

/// @brief The transfer-length field of an event's status word.
constexpr cinux::base::bit::BitRange kEventLength{.low = 0, .width = 24};

/// @brief Lays one control transfer's stages on a ring, status last.
// NOLINTNEXTLINE(readability-function-size) three stages named one by one
void control_stages(Trb* ring, RingState& state, uint64_t ring_physical, uint64_t setup,
                    uint32_t setup_extra, uint64_t buffer, uint32_t buffer_length, bool has_data,
                    uint32_t slot_bits, uint64_t& status_trb) {
    EnqueueCommand(
        ring, kTransferRingTrbs, state, ring_physical, setup, kSetupPacketBytes,
        TrbControl(kTrbSetupStage, setup_extra | kTrbImmediateData | kTrbChain | slot_bits));
    if (has_data) {
        EnqueueCommand(ring, kTransferRingTrbs, state, ring_physical, buffer, buffer_length,
                       TrbControl(kTrbDataStage, kTrbDirIn | kTrbChain | slot_bits));
    }
    status_trb = ring_physical + (state.index * sizeof(Trb));
    EnqueueCommand(ring, kTransferRingTrbs, state, ring_physical, 0, 0,
                   TrbControl(kTrbStatusStage, kTrbDirIn | kTrbInterruptOnCompletion | slot_bits));
}

}  // namespace

template <typename Owner>
bool UsbDevice<Owner>::address(XhciHost& host) {
    uint8_t slot = 0;
    if (!host.run_command(0, TrbControl(kTrbEnableSlot, 0), slot) || slot == 0) {
        return false;
    }
    slot_ = slot;
    host.seat_device(slot, this, &UsbDevice::deliver);

    const base::PhysAddr kInput  = mm::Pmm::self().allocate_page();
    const base::PhysAddr kOutput = mm::Pmm::self().allocate_page();
    const base::PhysAddr kEp0    = mm::Pmm::self().allocate_page();
    const base::PhysAddr kEp1    = mm::Pmm::self().allocate_page();
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kInput.raw)), 0, kPageBytes);
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kOutput.raw)), 0, kPageBytes);
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kEp0.raw)), 0, kPageBytes);
    base::SetBytes(base::PtrAt<void>(mm::DirectMapVirt(kEp1.raw)), 0, kPageBytes);
    input_page_                                                       = kInput.raw;
    output_page_                                                      = kOutput.raw;
    ep0_page_                                                         = kEp0.raw;
    ep1_page_                                                         = kEp1.raw;
    base::PtrAt<uint64_t>(mm::DirectMapVirt(host.device_page_))[slot] = output_page_;

    for (unsigned long candidate = 1; candidate <= host.max_ports(); ++candidate) {
        if ((host.port_status(candidate) & 1U) == 0) {
            continue;
        }
        uint8_t speed = 0;
        if (!host.reset_port(candidate, speed)) {
            return false;
        }
        port_  = static_cast<uint8_t>(candidate);
        speed_ = speed;
        break;
    }
    if (port_ == 0) {
        return false;
    }

    auto* const kContext = base::PtrAt<uint32_t>(mm::DirectMapVirt(input_page_));
    InitRing(base::PtrAt<Trb>(mm::DirectMapVirt(ep0_page_)), kTransferRingTrbs, ep0_page_);
    ep0_ = RingState{.index = 0, .cycle = true};
    SetAddFlags(kContext, 0x3U);
    BuildSlotContext(kContext + 8, 0, port_, speed_, 1);
    BuildControlEpContext(kContext + 16, ep0_page_,
                          speed_ == kSpeedHigh ? kPacketHigh : kPacketElse);
    const uint32_t kSlotBits = static_cast<uint32_t>(slot_) << 24U;
    uint8_t        answered  = 0;
    return host.run_command(input_page_, TrbControl(kTrbAddressDevice, kSlotBits), answered);
}

template <typename Owner>
bool UsbDevice<Owner>::spin_for_control(XhciHost& host) {
    for (unsigned long spin = 0; spin < kSpinLimit; ++spin) {
        host.drain_events();
        if (control_done_) {
            return control_code_ == kCompletionSuccess;
        }
        if (host.unhealthy()) {
            return false;
        }
        arch::CpuRelax();
    }
    return false;
}

template <typename Owner>
bool UsbDevice<Owner>::control_read(XhciHost& host, uint16_t value, void* data, uint16_t length) {
    auto* const    kRing   = base::PtrAt<Trb>(mm::DirectMapVirt(ep0_page_));
    const uint64_t kBuffer = ep0_page_ + kControlBufferOffset;
    const uint32_t kSlot   = static_cast<uint32_t>(slot_) << 24U;
    control_trb_           = 0;
    control_done_          = false;
    control_code_          = 0;
    control_stages(kRing, ep0_, ep0_page_, PackSetup(0x80, 0x06, value, 0, length),
                   kSetupDataIn << 17U, kBuffer, length, true, kSlot, control_trb_);
    host.write_doorbell(slot_, kControlEndpointId);
    if (!spin_for_control(host)) {
        return false;
    }
    base::CopyBytes(data, base::PtrAt<const void>(mm::DirectMapVirt(kBuffer)), length);
    return true;
}

template <typename Owner>
bool UsbDevice<Owner>::set_configuration(XhciHost& host, uint8_t config_value) {
    auto* const    kRing = base::PtrAt<Trb>(mm::DirectMapVirt(ep0_page_));
    const uint32_t kSlot = static_cast<uint32_t>(slot_) << 24U;
    control_trb_         = 0;
    control_done_        = false;
    control_code_        = 0;
    control_stages(kRing, ep0_, ep0_page_, PackSetup(0x00, 0x09, config_value, 0, 0),
                   kSetupNoData << 17U, 0, 0, false, kSlot, control_trb_);
    host.write_doorbell(slot_, kControlEndpointId);
    return spin_for_control(host);
}

template <typename Owner>
bool UsbDevice<Owner>::configure_interrupt_in(XhciHost& host, uint16_t max_packet,
                                              uint8_t interval) {
    if (interval == 0 || (speed_ >= kSpeedHigh && interval > 16)) {
        return false;
    }
    auto interval_exponent = static_cast<uint8_t>(interval - 1U);
    if (speed_ < kSpeedHigh) {
        interval_exponent = 3;
        for (unsigned period = interval; period > 1; period /= 2U) {
            ++interval_exponent;
        }
    }
    auto* const kContext = base::PtrAt<uint32_t>(mm::DirectMapVirt(input_page_));
    base::SetBytes(kContext, 0, kPageBytes);
    InitRing(base::PtrAt<Trb>(mm::DirectMapVirt(ep1_page_)), kTransferRingTrbs, ep1_page_);
    ep1_ = RingState{.index = 0, .cycle = true};
    SetAddFlags(kContext, 0x9U);
    BuildSlotContext(kContext + 8, 0, port_, speed_, kInterruptEndpointId);
    BuildInterruptInEpContext(kContext + 32, ep1_page_, max_packet, interval_exponent);
    const uint32_t kSlotBits = static_cast<uint32_t>(slot_) << 24U;
    uint8_t        answered  = 0;
    return host.run_command(input_page_, TrbControl(kTrbConfigureEndpoint, kSlotBits), answered);
}

template <typename Owner>
void UsbDevice<Owner>::ring_report(XhciHost& host) {
    auto* const kRing = base::PtrAt<Trb>(mm::DirectMapVirt(ep1_page_));
    report_trb_       = ep1_page_ + (ep1_.index * sizeof(Trb));
    EnqueueCommand(kRing, kTransferRingTrbs, ep1_, ep1_page_, ep1_page_ + kReportOffset,
                   kReportBytes, TrbControl(kTrbNormal, kTrbInterruptOnCompletion));
    host.write_doorbell(slot_, kInterruptEndpointId);
}

template <typename Owner>
void UsbDevice<Owner>::interrupt_read(XhciHost& host, ReportHandler handler, Owner* owner) {
    report_call_.arm(handler, owner);
    report_live_ = true;
    ring_report(host);
}

template <typename Owner>
void UsbDevice<Owner>::on_transfer(const Trb& event) {
    if (event.parameter == control_trb_ && control_trb_ != 0) {
        control_done_ = true;
        control_code_ = CompletionCode(event);
        control_trb_  = 0;
        return;
    }
    if (report_live_ && event.parameter == report_trb_) {
        report_live_         = false;
        report_trb_          = 0;
        const uint8_t  kCode = CompletionCode(event);
        const uint32_t kResidual =
            cinux::base::bit::BitMask<uint32_t>{event.status}.extract(kEventLength);
        const unsigned long kLength = kResidual <= kReportBytes ? kReportBytes - kResidual : 0;
        report_call_.invoke(kCode, kLength);
    }
}

template <typename Owner>
void UsbDevice<Owner>::deliver(void* device, const Trb& event) {
    static_cast<UsbDevice*>(device)->on_transfer(event);
}

template <typename Owner>
const unsigned char* UsbDevice<Owner>::report() const {
    return base::PtrAt<const unsigned char>(mm::DirectMapVirt(ep1_page_ + kReportOffset));
}

template <typename Owner>
uint8_t UsbDevice<Owner>::slot() const {
    return slot_;
}

}  // namespace cinux::driver::usb

namespace cinux::driver {
class UsbKeyboard;
}

template class cinux::driver::usb::UsbDevice<cinux::driver::UsbKeyboard>;
