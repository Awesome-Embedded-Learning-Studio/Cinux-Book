/**
 * @file kernel/drivers/usb/xhci_registers.hpp
 * @brief xHCI host-controller register layout (MMIO structs + bit constants)
 *
 * Pure types -- no MMIO access, host-compilable (static_asserts verify the
 * packed layouts).  Three MMIO blocks live in BAR0 at offsets read from the
 * capability registers:
 *   - Capability registers : BAR0 + 0x00          (XhciCapRegs)
 *   - Operational registers: BAR0 + CAPLENGTH      (XhciOpRegs)
 *   - Runtime registers    : BAR0 + RTSOFF         (XhciInterrupterRegs @ +0x20)
 *   - Doorbell array       : BAR0 + DBOFF          (one uint32 per slot)
 *   - Port registers       : op_base + 0x400 + port*0x10 (PORTSC @ +0x00)
 *
 * Bit constants I am confident in are defined here; the PORTSC speed / power
 * bits are added in Batch 3B (verified against QEMU behaviour) to avoid
 * guessing spec positions.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::usb {

// ============================================================
// Capability registers (BAR0 + 0x00, read-only)
// ============================================================

struct XhciCapRegs {
    volatile uint32_t
        cap_length_version;        ///< +0x00 [7:0] CAPLENGTH (op-reg offset), [31:16] HCIVERSION
    volatile uint32_t hcsparams1;  ///< +0x04 [7:0] MaxSlots, [18:8] MaxIntrs, [31:24] MaxPorts
    volatile uint32_t hcsparams2;  ///< +0x08 scratchpad / ERST params (see scratchpad_buf_count)
    volatile uint32_t hcsparams3;  ///< +0x0C U1/U2 exit latency
    volatile uint32_t hccparams1;  ///< +0x10 [0] AC64, [2] CSZ, [31:16] Extended Capabilities ptr
    volatile uint32_t dboff;       ///< +0x14 Doorbell array offset (from BAR0)
    volatile uint32_t rtsoff;      ///< +0x18 Runtime register offset (from BAR0)
    volatile uint32_t hccparams2;  ///< +0x1C
};
static_assert(sizeof(XhciCapRegs) == 0x20, "XhciCapRegs must be 0x20 bytes");

namespace Hcsparams1 {
constexpr uint32_t kMaxSlotsMask  = 0x000000FF;  ///< bits [7:0]
constexpr uint32_t kMaxIntrsShift = 8;
constexpr uint32_t kMaxIntrsMask  = 0x00000700;  ///< bits [18:8] (after shift, 0x7FF)
constexpr uint32_t kMaxPortsShift = 24;
constexpr uint32_t kMaxPortsMask  = 0xFF000000;  ///< bits [31:24]
}  // namespace Hcsparams1

namespace Hccparams1 {
constexpr uint32_t kAc64           = 1U << 0;  ///< 64-bit addressing capability
constexpr uint32_t kContextSize    = 1U << 2;  ///< 0 = 32-byte contexts, 1 = 64-byte
constexpr uint32_t kExtCapPtrShift = 16;
constexpr uint32_t kExtCapPtrMask  = 0xFFFF0000;  ///< bits [31:16]
}  // namespace Hccparams1

/// Decode HCSPARAMS2 into the number of scratchpad buffers the controller
/// requires (SPB is split: bits[4:0] lo, bits[20:16] hi, combined lo | hi<<5).
/// Pure (host-testable).
constexpr uint32_t scratchpad_buf_count(uint32_t hcsparams2) {
    const uint32_t lo = hcsparams2 & 0x1F;
    const uint32_t hi = (hcsparams2 >> 16) & 0x1F;
    return lo | (hi << 5);
}

// ============================================================
// Operational registers (at BAR0 + CAPLENGTH)
// ============================================================

struct XhciOpRegs {
    volatile uint32_t usbcmd;        ///< +0x00 USBCMD
    volatile uint32_t usbsts;        ///< +0x04 USBSTS
    volatile uint32_t pagesize;      ///< +0x08 PAGESIZE
    volatile uint32_t reserved0[2];  ///< +0x0C, +0x10
    volatile uint32_t dnctrl;        ///< +0x14 DNCTRL
    volatile uint32_t crcr_lo;       ///< +0x18 Command Ring Control (low)
    volatile uint32_t crcr_hi;       ///< +0x1C Command Ring Control (high)
    volatile uint32_t reserved1[4];  ///< +0x20..+0x2C
    volatile uint32_t dcbaap_lo;     ///< +0x30 Device Context Base Address Array (low)
    volatile uint32_t dcbaap_hi;     ///< +0x34 Device Context Base Address Array (high)
    volatile uint32_t config;        ///< +0x38 CONFIG (MaxSlotsEn [7:0])
};
static_assert(sizeof(XhciOpRegs) == 0x3C, "XhciOpRegs must be 0x3C bytes");

namespace Usbcmd {
constexpr uint32_t kRunStop   = 1U << 0;  ///< 0 = stop (Halt), 1 = run
constexpr uint32_t kHcReset   = 1U << 1;  ///< Host Controller Reset (HCRST)
constexpr uint32_t kIntEnable = 1U << 2;  ///< Interrupter enable (INTE)
}  // namespace Usbcmd

namespace Usbsts {
constexpr uint32_t kHcHalted           = 1U << 0;  ///< HCH: controller halted
constexpr uint32_t kHostSystemError    = 1U << 2;  ///< HSE
constexpr uint32_t kEventInterrupt     = 1U << 3;  ///< EINT: event interrupt pending
constexpr uint32_t kPortChange         = 1U << 4;  ///< PCD
constexpr uint32_t kControllerNotReady = 1U << 8;  ///< CNR: set during reset
}  // namespace Usbsts

namespace Crcr {
constexpr uint32_t kRingCycleState = 1U << 0;                ///< RCS (consumer cycle bit)
constexpr uint32_t kCmdAbort       = 1U << 2;                ///< CA
constexpr uint32_t kCmdStop        = 1U << 3;                ///< CS
constexpr uint32_t kRingRunning    = 1U << 3;                ///< CRR (read-only: ring running)
constexpr uint64_t kPtrMask        = 0xFFFFFFFFFFFFFFC0ULL;  ///< bits [63:6] ring address
}  // namespace Crcr

namespace Config {
constexpr uint32_t kMaxSlotsEnMask = 0x000000FF;  ///< bits [7:0] MaxSlotsEn
}  // namespace Config

// ============================================================
// Port registers (at op_base + 0x400 + port * 0x10)
// ============================================================

namespace PortRegs {
constexpr uint32_t kBaseOffset = 0x400;  ///< first port register set, from op base
constexpr uint32_t kSpacing    = 0x10;   ///< bytes between port register sets
}  // namespace PortRegs

namespace Portsc {
constexpr uint32_t kCurrentConnect = 1U << 0;  ///< CCS: device present
constexpr uint32_t kPortEnabled    = 1U << 1;  ///< PED
constexpr uint32_t kOverCurrent    = 1U << 3;  ///< OCA
constexpr uint32_t kPortReset      = 1U << 4;  ///< PR: set to reset the port

// Port Link State [8:5] (read current PM state; written with Link State Strobe).
constexpr uint32_t kLinkStateShift = 5;
constexpr uint32_t kLinkStateMask  = 0xFU << 5;
namespace LinkState {
constexpr uint32_t kU0       = 0;  ///< enabled, idle
constexpr uint32_t kPolling  = 7;  ///< reset / polling (device present, resetting)
constexpr uint32_t kHotReset = 9;
constexpr uint32_t kResume   = 0xF;
}  // namespace LinkState

constexpr uint32_t kPortPower  = 1U << 9;  ///< PP: port power (if HCC PPC)
constexpr uint32_t kSpeedShift = 10;       ///< device speed [13:10]
constexpr uint32_t kSpeedMask  = 0xFU << 10;
constexpr uint32_t kLinkStrobe = 1U << 16;  ///< set when writing the link state

// Change bits (write-1-to-clear).
constexpr uint32_t kConnectStatusChange = 1U << 17;  ///< CSC
constexpr uint32_t kPortEnableChange    = 1U << 18;  ///< PEC
constexpr uint32_t kPortResetChange     = 1U << 21;  ///< RC: 1->0 transition of PR
constexpr uint32_t kPortLinkStateChange = 1U << 22;  ///< PLC

/// Extract the Port Link State [8:5] from a PORTSC value.  Pure (host-testable).
constexpr uint32_t portsc_link_state(uint32_t portsc) {
    return (portsc & kLinkStateMask) >> kLinkStateShift;
}

/// Extract the device speed [13:10] from a PORTSC value (0=undef,1=FS,2=LS,
/// 3=HS,4=SS).  Matches the Slot Context dev_info speed encoding.  Pure.
constexpr uint32_t portsc_speed(uint32_t portsc) {
    return (portsc & kSpeedMask) >> kSpeedShift;
}
}  // namespace Portsc

// ============================================================
// Runtime / Interrupter registers (at BAR0 + RTSOFF; IR0 @ +0x20)
// ============================================================

/// One interrupter register set (0x20 bytes).  IR0 is at runtime_base + 0x20.
struct XhciInterrupterRegs {
    volatile uint32_t iman;       ///< +0x00 [0] IP (pending), [1] IE (enable)
    volatile uint32_t imod;       ///< +0x04 interrupt moderation (IMODC[31:16], IMODI[15:0])
    volatile uint32_t erstsz;     ///< +0x08 ERST Size ([15:0] = number of segments)
    volatile uint32_t reserved;   ///< +0x0C
    volatile uint32_t erstba_lo;  ///< +0x10 ERST base address (low)
    volatile uint32_t erstba_hi;  ///< +0x14 ERST base address (high)
    volatile uint32_t erdp_lo;    ///< +0x18 Event Ring Dequeue Pointer (low)
    volatile uint32_t erdp_hi;    ///< +0x1C Event Ring Dequeue Pointer (high)
};
static_assert(sizeof(XhciInterrupterRegs) == 0x20, "XhciInterrupterRegs must be 0x20 bytes");

namespace Iman {
constexpr uint32_t kPending = 1U << 0;  ///< IP: clear by writing 1
constexpr uint32_t kEnable  = 1U << 1;  ///< IE: enable this interrupter
}  // namespace Iman

namespace Erstsz {
constexpr uint32_t kSizeMask = 0x0000FFFF;  ///< bits [15:0] segment count
}  // namespace Erstsz

namespace Ertba {
// 64-bit ERST base; written as two 32-bit halves (lo then hi).
constexpr uint64_t kPtrMask = 0xFFFFFFFFFFFFFFC0ULL;  ///< bits [63:6]
}  // namespace Ertba

namespace Erdp {
constexpr uint32_t kEventHandlerBusy = 1U << 3;                ///< EHB (bit 3 of the low dword)
constexpr uint64_t kPtrMask          = 0xFFFFFFFFFFFFFFF0ULL;  ///< bits [63:4] dequeue ptr
}  // namespace Erdp

// ============================================================
// Doorbell (at BAR0 + DBOFF; one uint32 per slot, index 0 = command ring)
// ============================================================
namespace Doorbell {
/// Doorbell 0 target value to ring the command ring.
constexpr uint32_t kTargetCommandRing = 0;
}  // namespace Doorbell

}  // namespace cinux::drivers::usb
