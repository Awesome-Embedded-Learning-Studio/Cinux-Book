/**
 * @file kernel/drivers/usb/xhci_slot.hpp
 * @brief Per-device xHCI slot: device/input contexts + EP0 control ring
 *
 * Owns the DMA-backed contexts for one USB device and builds the input context
 * for the Address Device command (Batch 3B), consuming the 3A context encoders.
 * The device context is registered in the DCBAA by the caller (controller) via
 * device_context_phys(); the input context is fed to the Address Device command
 * via input_context_phys().  Control transfers (GET_DESCRIPTOR /
 * SET_CONFIGURATION) on the EP0 ring land in Batch 3C.
 *
 * Namespace: cinux::drivers::usb
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/dma/dma_buffer.hpp"
#include "usb_request.hpp"
#include "xhci_context.hpp"
#include "xhci_ring.hpp"

namespace cinux::drivers::usb {

class XHCIController;  // control transfers ring the EP doorbell via the controller

/// Endpoint ID (DCI) for EP0 -- the doorbell target + Transfer Event epid.
constexpr uint8_t kEp0Epid = 1;

class XhciSlot {
public:
    /// Allocate the device context (slot + EP0 = 64 B), input context (ICC +
    /// device-context copy = 96 B), and the EP0 control transfer ring.  Does NOT
    /// touch the DCBAA -- the caller registers device_context_phys() via
    /// XHCIController::dcbaa_set(slot_id, ...).  Error::OutOfMemory on alloc fail.
    cinux::lib::ErrorOr<void> allocate(uint8_t slot_id);

    /// Fill the input context for Address Device (add slot + EP0):
    ///   - Input Control Context: A0 | A1 (add slot, add EP0)
    ///   - Slot Context: route=0, speed, root-hub port, last_ctx=1 (EP0 only)
    ///   - EP0 Context: control EP, @p ep0_max_packet, EP0-ring dequeue + DCS
    /// @p ep0_max_packet: 8 for FS/LS (unknown until descriptor read), 64 for HS.
    void build_address_input(uint32_t speed, uint8_t rh_port, uint32_t ep0_max_packet);

    // ---- Control transfers on EP0 (Batch 3C) ----
    // Each enqueues the SETUP/Data/Status TRB chain on the EP0 ring, rings the
    // EP0 doorbell via the controller, and waits for the Transfer Event.
    // Completion code Success(1) or ShortPacket(13) is accepted as success.

    /// 3-stage control IN transfer (e.g. GET_DESCRIPTOR).  Data lands in
    /// data_virt(); @return actual bytes received, or an error.
    cinux::lib::ErrorOr<uint32_t> control_in(XHCIController& hc, const UsbSetup& setup,
                                             uint32_t len);

    /// 2-stage control OUT transfer with no data stage (e.g. SET_CONFIGURATION).
    cinux::lib::ErrorOr<void> control_out_no_data(XHCIController& hc, const UsbSetup& setup);

    /// Convenience: GET_DESCRIPTOR (IN) into data_virt(); @return actual bytes.
    cinux::lib::ErrorOr<uint32_t> get_descriptor(XHCIController& hc, uint8_t desc_type,
                                                 uint16_t index, uint32_t len);

    /// Convenience: SET_CONFIGURATION (OUT, no data stage).
    cinux::lib::ErrorOr<void> set_configuration(XHCIController& hc, uint8_t config_value);

    // ---- Interrupt-IN endpoint (generic transport; Batch 4A) ----
    // XhciSlot is bus transport only -- it configures + polls an interrupt-IN
    // endpoint without knowing what the device IS.  HID decode lives in the
    // caller (a device driver, e.g. UsbMouse in drivers/mouse/).

    /// Add an interrupt-IN endpoint via the Configure Endpoint command (TRB type
    /// 12).  @p ep_number from the endpoint descriptor; @p max_packet +
    /// @p interval from the descriptor.  Allocates the interrupt transfer ring.
    cinux::lib::ErrorOr<void> add_interrupt_endpoint(XHCIController& hc, uint8_t ep_number,
                                                     uint16_t max_packet, uint8_t interval);

    /// Poll the interrupt-IN endpoint once: enqueue a Normal TRB for @p buf_phys
    /// (length @p len), ring the doorbell, wait for the Transfer Event.  Returns
    /// the bytes transferred, or Error::TimedOut when the device is idle (NAK --
    /// correct interrupt-IN behaviour, not an error).
    cinux::lib::ErrorOr<uint32_t> poll_interrupt_in(XHCIController& hc, uint64_t buf_phys,
                                                    uint32_t len);

    /// Submit an interrupt-IN transfer asynchronously (Batch 5A): enqueue the
    /// Normal TRB + ring the doorbell, but do NOT wait for the Transfer Event.
    /// The controller completes it when the device sends a report; the event
    /// lands on the event ring and is dispatched to the slot's TransferListener
    /// in poll_events (hard-IRQ context).  An idle device keeps the transfer
    /// pending -- zero CPU.  This is the production input path; poll_interrupt_in
    /// above stays for the test kernel (which has no sti / CPU interrupts).
    void submit_interrupt_in_async(XHCIController& hc, uint64_t buf_phys, uint32_t len);

    /// Control-IN data buffer -- descriptor reads land here (valid after a
    /// successful control_in / get_descriptor).
    const uint8_t* data_virt() const { return static_cast<const uint8_t*>(data_buf_.virt()); }

    uint8_t  slot_id() const { return slot_id_; }
    uint64_t device_context_phys() const { return dev_ctx_buf_.phys(); }
    uint64_t input_context_phys() const { return in_ctx_buf_.phys(); }

    /// Device-context slot dword @p i (0..7) -- read the controller-written
    /// state after Address Device (DW3 dev_state [31:27] = slot state).
    uint32_t device_slot_dword(uint32_t i) const {
        return static_cast<volatile uint32_t*>(dev_ctx_buf_.virt())[i];
    }

    /// Input-context dword @p i (0..23: ICC 0..7, slot 8..15, EP0 16..23).
    uint32_t input_dword(uint32_t i) const {
        return static_cast<volatile uint32_t*>(in_ctx_buf_.virt())[i];
    }

    TrbRing& ep0_ring() { return ep0_ring_; }

private:
    uint8_t                        slot_id_ = 0;
    cinux::drivers::dma::DmaBuffer dev_ctx_buf_;   ///< slot + EP0 device context (output)
    cinux::drivers::dma::DmaBuffer in_ctx_buf_;    ///< ICC + device-context copy (input)
    cinux::drivers::dma::DmaBuffer ep0_ring_buf_;  ///< EP0 transfer ring storage
    cinux::drivers::dma::DmaBuffer data_buf_;      ///< control-IN data buffer (descriptors)
    cinux::drivers::dma::DmaBuffer int_ring_buf_;  ///< interrupt-IN transfer ring storage
    TrbRing                        ep0_ring_;
    TrbRing                        int_ring_;        ///< interrupt-IN ring
    uint8_t                        int_ep_dci_ = 0;  ///< interrupt-IN EP DCI (doorbell target)
};

}  // namespace cinux::drivers::usb
