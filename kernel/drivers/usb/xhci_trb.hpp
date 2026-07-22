/**
 * @file kernel/drivers/usb/xhci_trb.hpp
 * @brief xHCI Transfer Request Block (TRB) layout + type/bit constants
 *
 * Every command, transfer and event is a 16-byte TRB.  The Cycle bit (control
 * [0]) is the ring handshaking flag; TRB Type lives in control [15:10].  Ring
 * data structures (xhci_ring.hpp) layer the cycle-bit / Link-TRB mechanics on
 * top of an array of Trb.  Pure types -- host-compilable.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::usb {

/// One xHCI TRB (16 bytes).  Members are volatile: both the CPU and controller
/// access TRBs through a DMA region.
struct Trb {
    volatile uint64_t parameter;  ///< +0:  data pointer / command args / event params
    volatile uint32_t status;     ///< +8:  transfer length / completion code
    volatile uint32_t control;    ///< +12: [0]=Cycle, [15:10]=TRB Type, + flags
};
static_assert(sizeof(Trb) == 16, "TRB must be 16 bytes");

// ============================================================
// TRB Type values (control [15:10])
// ============================================================

namespace TrbType {
// Transfer-ring TRBs
constexpr uint32_t kNormal            = 1;  ///< Normal (bulk/interrupt data)
constexpr uint32_t kSetup             = 2;  ///< Setup Stage (control transfer)
constexpr uint32_t kData              = 3;  ///< Data Stage
constexpr uint32_t kStatus            = 4;  ///< Status Stage
constexpr uint32_t kLink              = 6;  ///< Link (ring closure)
constexpr uint32_t kNoOp              = 8;  ///< No-Op (transfer ring / simple command)
// Command-ring TRBs
constexpr uint32_t kEnableSlot        = 9;
constexpr uint32_t kAddressDevice     = 11;
constexpr uint32_t kConfigureEndpoint = 12;
// Event-ring TRBs (events produced by the controller)
constexpr uint32_t kTransferEvent     = 32;
constexpr uint32_t kCommandCompletion = 33;
constexpr uint32_t kPortStatusChange  = 34;
}  // namespace TrbType

// ============================================================
// Control-word bit constants
// ============================================================

// Verified against Linux drivers/usb/host/xhci.h (TRB_CYCLE/TC/ISP/CHAIN/IOC/IDT/DIR)
// and QEMU hw/usb/hcd-xhci.c (TRB_TR_*).  The 2A draft had IDT/IOC/CH at the wrong
// bits (1<<4 / 1<<2 / 1<<5); fixed in 3C (they were never exercised before control
// transfers, so 2A-2B-2C stayed green).
constexpr uint32_t kCycleBit              = 1U << 0;   ///< C: ownership handshake bit
constexpr uint32_t kToggleCycle           = 1U << 1;   ///< Link TRB TC: consumer flips cycle
constexpr uint32_t kInterruptOnShort      = 1U << 2;   ///< ISP: interrupt on short packet (IN)
constexpr uint32_t kChain                 = 1U << 4;   ///< CH: next TRB is part of this transfer
constexpr uint32_t kInterruptOnCompletion = 1U << 5;   ///< IOC: raise interrupt on completion
constexpr uint32_t kImmediateData         = 1U << 6;   ///< IDT: data inline in parameter
constexpr uint32_t kDataDirIn             = 1U << 16;  ///< DIR (Data/Status stage): 1 = IN
constexpr uint32_t kTrbTypeShift          = 10;

/// Build a control word from a TRB type + flags (no cycle bit -- the ring sets
/// that from the producer cycle state).
constexpr uint32_t trb_control(uint32_t type, uint32_t flags = 0) {
    return (type << kTrbTypeShift) | flags;
}

/// Extract the TRB type field from a control word.
inline uint32_t trb_type(uint32_t control) {
    return (control >> kTrbTypeShift) & 0x3F;
}

// ============================================================
// Command TRB slot-ID field + Address Device bit
// ============================================================

/// Slot ID field [31:24], shared by command TRBs (Enable Slot / Address Device /
// Configure Endpoint carry the target slot) and Command Completion Events.
constexpr uint32_t kSlotIdShift = 24;

/// Build the slot-ID field for a command TRB control word.
constexpr uint32_t slot_id_for_trb(uint32_t slot_id) {
    return (slot_id & 0xFF) << kSlotIdShift;
}

/// Block Set Address Request (Address Device command control [9]).  BSR=0
/// completes the address stage (device -> Addressed); BSR=1 blocks at Default
/// until a SET_ADDRESS request.  xHCI host drivers use BSR=0.
constexpr uint32_t kBlockSetAddress = 1U << 9;

// ============================================================
// Command Completion Event parsing (event TRBs produced by the controller)
// ============================================================

/// Completion Code values (CCE status [31:24]).
namespace CompCode {
constexpr uint32_t kSuccess          = 1;  ///< command succeeded
constexpr uint32_t kTrbError         = 5;  ///< malformed / invalid parameter TRB
constexpr uint32_t kStallError       = 6;
constexpr uint32_t kResourceError    = 7;  ///< no slot / bandwidth available
constexpr uint32_t kNoSlotsAvailable = 9;
constexpr uint32_t kShortPacket      = 13;
}  // namespace CompCode

/// Slot ID returned by the completed command (CCE control [31:24]).  Enable
/// Slot reports the newly assigned slot ID here.
constexpr uint32_t cmd_completion_slot_id(uint32_t control) {
    return (control >> kSlotIdShift) & 0xFF;
}

/// Completion code (CCE status [31:24]); CompCode::kSuccess == 1.
constexpr uint32_t cmd_completion_code(uint32_t status) {
    return (status >> 24) & 0xFF;
}

// ---- Transfer Event parsing (EP transfer completion, type 32) ----
// Layout (QEMU xhci_write_event + Linux handle_tx_event): control [31:24] =
// slot ID (reuse cmd_completion_slot_id), [20:16] = endpoint ID; status
// [31:24] = completion code (reuse cmd_completion_code), [23:0] = REMAINING
// (untransferred) bytes -> actual = requested_length - remaining.
constexpr uint32_t transfer_event_epid(uint32_t control) {
    return (control >> 16) & 0x1F;
}
constexpr uint32_t transfer_event_remaining(uint32_t status) {
    return status & 0xFFFFFFU;
}

// ============================================================
// Control-transfer stage TRB builders (Batch 3C) -- pure encoders.
// Verified against Linux xhci-ring.c queue_setup/queue_status + QEMU
// hcd-xhci.c.  TRT (Transfer Type) lives at control [17:16] (Linux layout).
// QEMU qemu-xhci ignores TRT (routes direction from the SETUP bmRequestType),
// so TRT is decorative on QEMU but spec-correct for real xHCI 1.0 hardware.
// None of these include the cycle bit -- the ring adds it on enqueue.
// ============================================================

/// SETUP Stage transfer length (always 8 = the SETUP packet size).
constexpr uint32_t kSetupStageLength = 8;

/// Transfer Type (TRT) for the SETUP Stage TRB [17:16].
namespace Trt {
constexpr uint32_t kNone = 0;  ///< no data stage (wLength == 0)
constexpr uint32_t kOut  = 2;  ///< OUT data stage (host -> device)
constexpr uint32_t kIn   = 3;  ///< IN data stage (device -> host)
}  // namespace Trt

/// SETUP Stage TRB control word (type 2).  IDT (8-byte packet inline) + CHAIN
/// (data/status follow) + TRT at [17:16].
constexpr uint32_t setup_stage_control(uint32_t trt) {
    return trb_control(TrbType::kSetup, kImmediateData | kChain) | (trt << 16);
}

/// Data Stage TRB control word (type 3).  CHAIN (status follows).  IN data sets
/// DIR (device -> host) + ISP (short-packet event).  No IOC.
constexpr uint32_t data_stage_control(bool in) {
    const uint32_t flags = kChain | (in ? (kDataDirIn | kInterruptOnShort) : 0U);
    return trb_control(TrbType::kData, flags);
}

/// Status Stage TRB control word (type 4).  IOC (event on completion).  The
/// status handshake direction is the OPPOSITE of the data stage: an IN-data
/// transfer (GET_DESCRIPTOR) -> status OUT (no DIR); an OUT/no-data transfer
/// (SET_CONFIGURATION) -> status IN (DIR set).
constexpr uint32_t status_stage_control(bool data_in) {
    return trb_control(TrbType::kStatus, kInterruptOnCompletion | (data_in ? 0U : kDataDirIn));
}

/// Normal TRB (type 1) for an interrupt-IN poll (Batch 4A): IOC (event on
/// completion) + ISP (short-packet event).  NO DIR bit -- direction comes from
/// the EP-context type (kIntIn).  No cycle bit (the ring adds it).
constexpr uint32_t interrupt_in_trb_control() {
    return trb_control(TrbType::kNormal, kInterruptOnCompletion | kInterruptOnShort);
}

/// Device Context Index (DCI) for an endpoint -- the doorbell target + the
/// input-context EP-context slot.  EP0 = 1; EPn-OUT = 2n; EPn-IN = 2n+1.
constexpr uint32_t ep_dci(uint32_t ep_number, bool in) {
    return ep_number * 2 + (in ? 1u : 0u);
}

}  // namespace cinux::drivers::usb
