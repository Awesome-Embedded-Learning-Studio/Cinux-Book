/**
 * @file kernel/drivers/usb/xhci_slot.cpp
 * @brief XhciSlot: allocate DMA contexts + build the Address Device input context
 *
 * Batch 3B.  Consumes the 3A context encoders (slot_dev_info / ep_info2 /
 * ep_dequeue_ptr / input_add_flag) to fill the input context for Address Device.
 * All bit positions verified against Linux drivers/usb/host/xhci.h.
 *
 * Input Context layout (32-byte contexts, HCCPARAMS1 CSZ=0 -- the QEMU case):
 *   DW 0..7  : Input Control Context  (DW0 = add-flags)
 *   DW 8..15 : Slot Context copy      (DW0 dev_info, DW1 dev_info2)
 *   DW16..23 : EP0 Endpoint Context   (DW1 ep_info2, DW2/3 deq, DW4 tx_info)
 *
 * Namespace: cinux::drivers::usb
 */

#include "xhci_slot.hpp"

#include <stdint.h>

#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/lib/kprintf.hpp"
#include "xhci_controller.hpp"

namespace cinux::drivers::usb {

namespace {
// Device context: slot (ctx0) + EP0 (ctx1) + EP1-out (ctx2) + EP1-in (ctx3) = 4
// contexts.  Enlarged in 4A so Configure Endpoint can add the interrupt-IN EP
// (ctx3) without overflowing (3B used slot+EP0 = 64 B).
constexpr uint64_t kDevCtxBytes  = 128;  ///< slot + EP0 + EP1-out + EP1-in (4 contexts)
// Input context: ICC (32) + device-context copy through ctx3 = 32 + 4*32 = 160.
constexpr uint64_t kInCtxBytes   = 160;  ///< ICC + slot + EP0 + EP1-out + EP1-in
constexpr uint64_t kDataBufBytes = 256;  ///< control-IN data (device + config descriptors)
constexpr uint32_t kEp0RingTrbs  = 16;   ///< EP0 control ring depth (+1 Link TRB)
constexpr uint32_t kEp0AvgTrbLen = 8;    ///< average TRB length for control EP
constexpr uint32_t kIntRingTrbs  = 16;   ///< interrupt-IN ring depth (+1 Link TRB)

void zero_bytes(void* p, uint64_t bytes) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < bytes; ++i) {
        b[i] = 0;
    }
}
}  // namespace

cinux::lib::ErrorOr<void> XhciSlot::allocate(uint8_t slot_id) {
    slot_id_ = slot_id;

    auto d = cinux::drivers::dma::g_dma_pool.alloc(kDevCtxBytes);
    if (!d.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    dev_ctx_buf_ = std::move(d.value());
    zero_bytes(dev_ctx_buf_.virt(), kDevCtxBytes);

    auto ic = cinux::drivers::dma::g_dma_pool.alloc(kInCtxBytes);
    if (!ic.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    in_ctx_buf_ = std::move(ic.value());
    zero_bytes(in_ctx_buf_.virt(), kInCtxBytes);

    auto er = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kEp0RingTrbs + 1) * 16);
    if (!er.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    ep0_ring_buf_ = std::move(er.value());
    zero_bytes(ep0_ring_buf_.virt(), static_cast<uint64_t>(kEp0RingTrbs + 1) * 16);
    ep0_ring_.init(static_cast<Trb*>(ep0_ring_buf_.virt()), kEp0RingTrbs, ep0_ring_buf_.phys());

    auto db = cinux::drivers::dma::g_dma_pool.alloc(kDataBufBytes);
    if (!db.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    data_buf_ = std::move(db.value());
    zero_bytes(data_buf_.virt(), kDataBufBytes);

    auto ir = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kIntRingTrbs + 1) * 16);
    if (!ir.ok()) {
        return cinux::lib::Error::OutOfMemory;
    }
    int_ring_buf_ = std::move(ir.value());
    zero_bytes(int_ring_buf_.virt(), static_cast<uint64_t>(kIntRingTrbs + 1) * 16);
    int_ring_.init(static_cast<Trb*>(int_ring_buf_.virt()), kIntRingTrbs, int_ring_buf_.phys());

    return {};
}

void XhciSlot::build_address_input(uint32_t speed, uint8_t rh_port, uint32_t ep0_max_packet) {
    auto* in = static_cast<volatile uint32_t*>(in_ctx_buf_.virt());

    // Input Control Context (DW0..7): DW0 = Drop flags (nothing dropped),
    // DW1 = Add flags (A0 = add slot, A1 = add EP0).  Per xHCI spec / Linux
    // xhci_input_ctrl_ctx, Drop precedes Add (verified against QEMU hcd-xhci,
    // which requires ictx DW0==0 && DW1==0x3 for Address Device).
    in[0] = 0;
    in[1] = input_add_flag(0) | input_add_flag(1);

    // Slot Context (DW8..15): route=0, speed, last_ctx=1 (EP0 only), root-hub port.
    in[8] = slot_dev_info(0, speed, 1);
    in[9] = slot_dev_info2(0, rh_port);
    // DW10 tt_info and DW11 dev_state stay 0 (controller assigns the address).

    // EP0 Endpoint Context (DW16..23): control EP, max packet, EP0-ring dequeue.
    in[17]             = ep_info2(EpType::kControl, ep0_max_packet);
    const uint64_t deq = ep_dequeue_ptr(ep0_ring_buf_.phys(), true);
    in[18]             = static_cast<uint32_t>(deq);
    in[19]             = static_cast<uint32_t>(deq >> 32);
    in[20]             = ep_tx_info(kEp0AvgTrbLen);
}

// ============================================================
// Control transfers on EP0 (Batch 3C)
// TRB stage encoders + completion parsing are in xhci_trb.hpp (verified against
// Linux xhci-ring.c + QEMU hcd-xhci.c).  The cycle bit is added by TrbRing on
// enqueue, so the control words here never include it.
// ============================================================

cinux::lib::ErrorOr<uint32_t> XhciSlot::control_in(XHCIController& hc, const UsbSetup& setup,
                                                   uint32_t len) {
    if (len > kDataBufBytes) {
        return cinux::lib::Error::InvalidArgument;
    }
    // 3-stage control IN: SETUP (TRT=IN) -> Data (IN) -> Status (OUT handshake).
    ep0_ring_.enqueue(setup_to_u64(setup), kSetupStageLength, setup_stage_control(Trt::kIn));
    ep0_ring_.enqueue(data_buf_.phys(), len, data_stage_control(true));
    ep0_ring_.enqueue(0, 0, status_stage_control(true));

    auto ev = hc.run_transfer(slot_id_, kEp0Epid);
    if (!ev.ok()) {
        return ev.error();
    }
    const uint32_t code = cmd_completion_code(ev.value().status);
    if (code != CompCode::kSuccess && code != CompCode::kShortPacket) {
        cinux::lib::kprintf("[xHCI] control_in failed: completion code=%u\n", code);
        return cinux::lib::Error::IOError;
    }
    return len - transfer_event_remaining(ev.value().status);  // actual bytes received
}

cinux::lib::ErrorOr<void> XhciSlot::control_out_no_data(XHCIController& hc, const UsbSetup& setup) {
    // 2-stage control OUT (no data): SETUP (TRT=none) -> Status (IN handshake).
    ep0_ring_.enqueue(setup_to_u64(setup), kSetupStageLength, setup_stage_control(Trt::kNone));
    ep0_ring_.enqueue(0, 0, status_stage_control(false));

    auto ev = hc.run_transfer(slot_id_, kEp0Epid);
    if (!ev.ok()) {
        return ev.error();
    }
    const uint32_t code = cmd_completion_code(ev.value().status);
    if (code != CompCode::kSuccess) {
        cinux::lib::kprintf("[xHCI] control_out failed: completion code=%u\n", code);
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<uint32_t> XhciSlot::get_descriptor(XHCIController& hc, uint8_t desc_type,
                                                       uint16_t index, uint32_t len) {
    return control_in(hc, make_get_descriptor_setup(desc_type, index, len), len);
}

cinux::lib::ErrorOr<void> XhciSlot::set_configuration(XHCIController& hc, uint8_t config_value) {
    const UsbSetup s =
        make_setup(bm_request_type(UsbDir::kOut, UsbReqType::kStandard, UsbRecipient::kDevice),
                   UsbRequest::kSetConfiguration, config_value, 0, 0);
    return control_out_no_data(hc, s);
}

// ============================================================
// Interrupt-IN endpoint -- generic transport (Batch 4A/4B refactor).
// Verified against the 4A workflow spec (QEMU hcd-xhci.c xhci_configure_slot
// + Linux xhci-ring.c).  XhciSlot configures + polls an interrupt-IN EP
// without interpreting the payload; HID decode is the caller's job.
// ============================================================

cinux::lib::ErrorOr<void> XhciSlot::add_interrupt_endpoint(XHCIController& hc, uint8_t ep_number,
                                                           uint16_t max_packet, uint8_t interval) {
    const uint32_t dci = ep_dci(ep_number, true);  // interrupt-IN DCI = ep*2 + 1
    int_ep_dci_        = static_cast<uint8_t>(dci);
    zero_bytes(in_ctx_buf_.virt(), kInCtxBytes);  // clear the stale Address-Device input context

    auto* in = static_cast<volatile uint32_t*>(in_ctx_buf_.virt());
    // Input Control Context: drop=0, add = A0 (slot, to update Context Entries)
    // | A_ep.  EP0 (A1) must stay CLEAR (already exists) -- QEMU rejects otherwise.
    in[0]    = 0;
    in[1]    = input_add_flag(0) | input_add_flag(dci);

    // Slot context (in[8..15]): preserve route/speed/rh_port from the addressed
    // device context; update Context Entries (last_ctx [31:27]) to the new DCI.
    uint32_t dev_info = device_slot_dword(0);
    dev_info          = (dev_info & ~(0x1FU << 27)) | (dci << 27);
    in[8]             = dev_info;
    in[9]             = device_slot_dword(1);  // dev_info2 preserved

    // EP context at in[8 + 8*dci ..] (input offset 32 + 32*dci).  Host writes
    // EP_STATE=disabled; the controller flips it to running.  Interval is the
    // descriptor bInterval taken directly (full-speed rule).
    const uint32_t base = 8 + 8 * dci;
    in[base + 0]        = ep_info(EpState::kDisabled, interval, 0);
    in[base + 1]        = ep_info2(EpType::kIntIn, max_packet, 3, 0);
    const uint64_t deq  = ep_dequeue_ptr(int_ring_buf_.phys(), true);
    in[base + 2]        = static_cast<uint32_t>(deq);
    in[base + 3]        = static_cast<uint32_t>(deq >> 32);
    in[base + 4]        = ep_tx_info(max_packet, max_packet);

    // Configure Endpoint command (TRB type 12).
    auto ev = hc.run_command(in_ctx_buf_.phys(), 0,
                             trb_control(TrbType::kConfigureEndpoint) | slot_id_for_trb(slot_id_));
    if (!ev.ok()) {
        return ev.error();
    }
    const uint32_t code = cmd_completion_code(ev.value().status);
    if (code != CompCode::kSuccess) {
        cinux::lib::kprintf("[xHCI] configure endpoint failed: completion code=%u\n", code);
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<uint32_t> XhciSlot::poll_interrupt_in(XHCIController& hc, uint64_t buf_phys,
                                                          uint32_t len) {
    if (int_ep_dci_ == 0) {
        return cinux::lib::Error::IOError;  // no interrupt endpoint configured
    }
    // Normal TRB (type 1): caller's buffer + length, IOC + ISP.
    int_ring_.enqueue(buf_phys, len, interrupt_in_trb_control());
    auto ev = hc.run_transfer(slot_id_, int_ep_dci_);
    if (!ev.ok()) {
        return ev.error();  // TimedOut = idle device NAK -- correct interrupt-IN behaviour
    }
    const uint32_t code = cmd_completion_code(ev.value().status);
    if (code != CompCode::kSuccess && code != CompCode::kShortPacket) {
        cinux::lib::kprintf("[xHCI] interrupt-IN poll failed: completion code=%u\n", code);
        return cinux::lib::Error::IOError;
    }
    return len - transfer_event_remaining(ev.value().status);  // bytes transferred
}

void XhciSlot::submit_interrupt_in_async(XHCIController& hc, uint64_t buf_phys, uint32_t len) {
    // Same enqueue + doorbell as poll_interrupt_in, but no run_transfer wait.
    // The Transfer Event completes asynchronously: it reaches poll_events (via
    // the xHCI MSI-X interrupt) which dispatches it to the slot's
    // TransferListener (on_transfer_complete) -- that decodes the report and
    // re-arms by calling this again.  Idle devices NAK and keep the transfer
    // pending, so no event fires and no CPU is spent.
    int_ring_.enqueue(buf_phys, len, interrupt_in_trb_control());
    hc.ring_doorbell(slot_id_, int_ep_dci_);
}

}  // namespace cinux::drivers::usb
