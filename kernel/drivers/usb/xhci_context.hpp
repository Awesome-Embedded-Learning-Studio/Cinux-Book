/**
 * @file kernel/drivers/usb/xhci_context.hpp
 * @brief xHCI Slot / Endpoint / Input-context layouts + field encoders
 *
 * Contexts are the controller's per-device and per-endpoint state, stored in DMA
 * memory as 32-byte (8-dword) blocks when HCCPARAMS1 CSZ=0 (the QEMU qemu-xhci
 * case; CSZ=1 uses 64-byte contexts).  This header provides:
 *   - 32-byte storage structs (members volatile; they are shared with the HC)
 *   - pure field-encoder functions that build a dword / u64 from logical params
 *
 * All bit positions are verified against the xHCI spec via Linux
 * drivers/usb/host/xhci.h (ROUTE_STRING / DEV_SPEED / LAST_CTX / EP_TYPE /
 * MAX_PACKET / TR_DEQ_PTR_MASK etc.) -- not guessed.  Pure encoders are
 * host-testable; the kernel writes them into DmaBuffer-backed contexts in
 * Batch 3B (XhciSlot, Address Device).
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::usb {

// ============================================================
// Value enumerations (verified encodings)
// ============================================================

/// Device speed (PORTSC speed field; also Slot Context dev_info [23:20]).
namespace UsbSpeed {
constexpr uint32_t kFull      = 1;  ///< 12 Mb/s
constexpr uint32_t kLow       = 2;  ///< 1.5 Mb/s
constexpr uint32_t kHigh      = 3;  ///< 480 Mb/s
constexpr uint32_t kSuper     = 4;  ///< 5 Gb/s
constexpr uint32_t kSuperPlus = 5;
}  // namespace UsbSpeed

/// Slot state (Slot Context dev_state [31:27]).
namespace SlotState {
constexpr uint32_t kDefault    = 1;
constexpr uint32_t kAddressed  = 2;
constexpr uint32_t kConfigured = 3;
}  // namespace SlotState

/// Endpoint state (Endpoint Context ep_info [2:0]).
namespace EpState {
constexpr uint32_t kDisabled = 0;
constexpr uint32_t kRunning  = 1;
constexpr uint32_t kHalted   = 2;
constexpr uint32_t kStopped  = 3;
constexpr uint32_t kError    = 4;
}  // namespace EpState

/// Endpoint type (Endpoint Context ep_info2 [6:3]).
namespace EpType {
constexpr uint32_t kIsocOut = 1;
constexpr uint32_t kBulkOut = 2;
constexpr uint32_t kIntOut  = 3;
constexpr uint32_t kControl = 4;
constexpr uint32_t kIsocIn  = 5;
constexpr uint32_t kBulkIn  = 6;
constexpr uint32_t kIntIn   = 7;
}  // namespace EpType

// ============================================================
// 32-byte context storage (8 dwords; shared DMA with the controller)
// ============================================================

/// Slot Context: dev_info / dev_info2 / tt_info / dev_state + 4 reserved dwords.
struct SlotContext {
    volatile uint32_t dw[8];
};
static_assert(sizeof(SlotContext) == 32, "Slot context must be 32 bytes");

/// Endpoint Context: ep_info / ep_info2 / deq(64) / tx_info + 3 reserved dwords.
struct EndpointContext {
    volatile uint32_t dw[8];
};
static_assert(sizeof(EndpointContext) == 32, "Endpoint context must be 32 bytes");

/// Input Control Context: DW0 = drop-flags (D0..D31), DW1 = add-flags
/// (A0..A31), DW2..7 reserved.  Precedes the device context in an Input Context
/// (Address Device / Configure Endpoint).  Drop precedes Add -- matches Linux
/// xhci_input_ctrl_ctx { drop_flags; add_flags; } and QEMU hcd-xhci (which
/// requires ictx DW0==0 && DW1==0x3 for Address Device).
struct InputControlContext {
    volatile uint32_t dw[8];
};
static_assert(sizeof(InputControlContext) == 32, "Input control context must be 32 bytes");

// ============================================================
// Pure field encoders (build a dword / u64 from logical params)
// ============================================================

// ---- Slot Context dev_info (DW0) ----
// [19:0] Route String | [23:20] Speed | [25] MTT | [26] Hub | [31:27] LastCtx
// @p last_ctx = index of the last valid endpoint context (1 for EP0-only device).
constexpr uint32_t slot_dev_info(uint32_t route, uint32_t speed, uint32_t last_ctx,
                                 bool mtt = false, bool hub = false) {
    return (route & 0xFFFFF) | ((speed & 0xF) << 20) | (mtt ? (1U << 25) : 0U) |
           (hub ? (1U << 26) : 0U) | ((last_ctx & 0x1F) << 27);
}

// ---- Slot Context dev_info2 (DW1) ----
// [15:0] Max Exit Latency | [23:16] Root Hub Port | [31:24] Max Ports (hub)
constexpr uint32_t slot_dev_info2(uint32_t max_exit_latency, uint32_t rh_port,
                                  uint32_t max_ports = 0) {
    return (max_exit_latency & 0xFFFF) | ((rh_port & 0xFF) << 16) | ((max_ports & 0xFF) << 24);
}

// ---- Slot Context dev_state (DW3) ----
// [7:0] USB Device Address | [31:27] Slot State
constexpr uint32_t slot_dev_state(uint32_t dev_addr, uint32_t slot_state) {
    return (dev_addr & 0xFF) | ((slot_state & 0x1F) << 27);
}

// ---- Endpoint Context ep_info2 (DW1) ----
// [2:1] CErr | [6:3] EP Type | [15:8] Max Burst | [31:16] Max Packet Size
constexpr uint32_t ep_info2(uint32_t ep_type, uint32_t max_packet, uint32_t cerr = 3,
                            uint32_t max_burst = 0) {
    return ((cerr & 0x3) << 1) | ((ep_type & 0x7) << 3) | ((max_burst & 0xFF) << 8) |
           ((max_packet & 0xFFFF) << 16);
}

// ---- Endpoint Context ep_info (DW0) ----
// [2:0] EP State | [9:8] Mult | [23:16] Interval
constexpr uint32_t ep_info(uint32_t ep_state, uint32_t interval = 0, uint32_t mult = 0) {
    return (ep_state & 0x7) | ((mult & 0x3) << 8) | ((interval & 0xFF) << 16);
}

// ---- Endpoint Context deq (DW2/DW3, 64-bit) ----
// [63:4] TR Dequeue Pointer (16-byte-aligned ring address) | [0] DCS
constexpr uint64_t ep_dequeue_ptr(uint64_t ring_phys, bool dcs = true) {
    return (ring_phys & 0xFFFFFFFFFFFFFFF0ULL) | (dcs ? 1ULL : 0ULL);
}

// ---- Endpoint Context tx_info (DW4) ----
// [15:0] Average TRB Length | [31:16] Max ESIT Payload (low)
constexpr uint32_t ep_tx_info(uint32_t avg_trb_len, uint32_t max_esit_payload = 0) {
    return (avg_trb_len & 0xFFFF) | ((max_esit_payload & 0xFFFF) << 16);
}

// ---- Input Control Context: add-flag bit ----
// Context index i sets bit i: 0 = slot, 1 = EP0, 2 = EP1-out, 3 = EP1-in, ...
constexpr uint32_t input_add_flag(uint32_t ctx_index) {
    return 1U << (ctx_index & 0x1F);
}

}  // namespace cinux::drivers::usb
