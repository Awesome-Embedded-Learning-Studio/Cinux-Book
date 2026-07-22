/**
 * @file kernel/drivers/usb/xhci_controller.cpp
 * @brief xHCI controller bring-up: PCI enable + BAR0 map + halt + reset
 *
 * Batch 1C scope.  No rings/interrupts yet -- those land in 2A/2B/2C.  The
 * reset path is verified in QEMU (run-kernel-test-xhci): find_xhci finds the
 * qemu-xhci, init resets it (CNR clears, controller halts), MaxPorts > 0.
 *
 * Namespace: cinux::drivers::usb
 */

#include "xhci_controller.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/dma/dma_pool.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pci/pci_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::drivers::usb {

XHCIController* XHCIController::s_instance_ = nullptr;

XHCIController& XHCIController::instance() {
    return *s_instance_;
}

void XHCIController::set_instance(XHCIController* c) {
    s_instance_ = c;
}

namespace {
// MMIO sub-allocation inside the KMEM_MMIO window (AHCI @+0x0, LAPIC @+0x10000,
// IOAPIC @+0x11000, xHCI BAR0 @+0x20000 (4 pages: cap/op/runtime/doorbell),
// MSI-X Table @+0x40000, PBA @+0x41000).  The MSI-X slots sit OUTSIDE xHCI
// BAR0's 4 pages to avoid colliding with the runtime regs (@BAR0+RTSOFF) and
// doorbells (@BAR0+DBOFF) -- see the GOTCHA in msix_controller.cpp (Batch 2C).
constexpr uint64_t kXhciMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x20000;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
constexpr uint64_t kMmioPages  = 4;
constexpr uint32_t kResetIters = 100000;  // bounded poll cap (QEMU completes in <<100)
constexpr uint64_t kPageSize   = 4096;

// Ring sizes (single page each via DmaPool).
constexpr uint32_t kCommandRingTrbs = 64;  // usable; +1 Link -> 65 TRBs = 1040 B (1 page)
constexpr uint32_t kEventRingTrbs   = 64;

void zero_bytes(void* p, uint64_t bytes) {
    auto* b = static_cast<uint8_t*>(p);
    for (uint64_t i = 0; i < bytes; ++i) {
        b[i] = 0;
    }
}

/// One Event Ring Segment Table entry: base address (64-bit) + size (16-bit) + reserved.
struct ErstEntry {
    uint64_t base;
    uint16_t size;
    uint8_t  reserved[6];
};
static_assert(sizeof(ErstEntry) == 16, "ERST entry must be 16 bytes");
}  // namespace

cinux::lib::ErrorOr<void> XHCIController::init(const pci::PCIDevice& dev) {
    dev_               = dev;  // retained for MSI-X setup in start()
    // 1. Enable PCI Bus Master (DMA) + Memory Space (MMIO response).
    const uint32_t cmd = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND,
                        cmd | pci::PciCmd::BUS_MASTER | pci::PciCmd::MEM_SPACE);

    // 2. Map BAR0 into the MMIO window.
    const uint64_t bar0 = dev.bar[0];
    for (uint64_t i = 0; i < kMmioPages; ++i) {
        if (!cinux::mm::g_vmm.map(kXhciMmioVirt + i * kPageSize, bar0 + i * kPageSize,
                                  kMmioFlags)) {
            return cinux::lib::Error::OutOfMemory;
        }
    }

    cap_regs_              = reinterpret_cast<XhciCapRegs*>(kXhciMmioVirt);
    const uint32_t cap_len = cap_regs_->cap_length_version & 0xFF;
    op_regs_               = reinterpret_cast<XhciOpRegs*>(kXhciMmioVirt + cap_len);
    doorbells_             = reinterpret_cast<volatile uint32_t*>(kXhciMmioVirt + cap_regs_->dboff);
    ir0_ = reinterpret_cast<XhciInterrupterRegs*>(kXhciMmioVirt + cap_regs_->rtsoff + 0x20);

    const uint32_t hcs1 = cap_regs_->hcsparams1;
    max_slots_          = static_cast<uint8_t>(hcs1 & 0xFF);
    max_ports_          = static_cast<uint8_t>((hcs1 >> 24) & 0xFF);

    cinux::lib::kprintf(
        "[xHCI] BAR0=0x%lx CAPLENGTH=%u MaxSlots=%u MaxPorts=%u DBOFF=0x%x RTSOFF=0x%x\n",
        static_cast<unsigned long>(bar0), cap_len, max_slots_, max_ports_, cap_regs_->dboff,
        cap_regs_->rtsoff);

    // 3. Halt (USBCMD=0, wait HCH), then reset (HCRST, wait CNR clear + HCRST
    //    self-clear).  BIOS/SMM legacy handoff is skipped -- QEMU qemu-xhci
    //    boots OS-owned; real HW needs the USB Legacy Support ext-cap handoff.
    op_regs_->usbcmd = 0;
    for (uint32_t i = 0; i < kResetIters; ++i) {
        if (op_regs_->usbsts & Usbsts::kHcHalted) {
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }

    op_regs_->usbcmd = Usbcmd::kHcReset;
    for (uint32_t i = 0; i < kResetIters; ++i) {
        const uint32_t sts = op_regs_->usbsts;
        const uint32_t run = op_regs_->usbcmd;
        if (!(sts & Usbsts::kControllerNotReady) && !(run & Usbcmd::kHcReset)) {
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }

    // Post-reset: controller must be halted with CNR clear.
    const uint32_t final_sts = op_regs_->usbsts;
    if (!(final_sts & Usbsts::kHcHalted) || (final_sts & Usbsts::kControllerNotReady)) {
        cinux::lib::kprintf("[xHCI] reset failed: USBSTS=0x%x\n", final_sts);
        return cinux::lib::Error::TimedOut;
    }

    cinux::lib::kprintf("[xHCI] controller reset complete (halted, CNR clear)\n");
    return {};
}

cinux::lib::ErrorOr<void> XHCIController::start() {
    // 1. DCBAA: (MaxSlots+1) 64-bit pointers; slot 0 holds the scratchpad
    //    buffer array address (0 if no scratchpad).
    const uint32_t dcbaa_entries = static_cast<uint32_t>(max_slots_) + 1;
    {
        auto b = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(dcbaa_entries) * 8);
        if (!b.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        dcbaa_buf_ = std::move(b.value());
        zero_bytes(dcbaa_buf_.virt(), static_cast<uint64_t>(dcbaa_entries) * 8);
        const uint64_t phys = dcbaa_buf_.phys();
        op_regs_->dcbaap_lo = static_cast<uint32_t>(phys);
        op_regs_->dcbaap_hi = static_cast<uint32_t>(phys >> 32);
    }

    // 2. Scratchpad buffers (if HCSPARAMS2 requests any).
    const uint32_t spb = scratchpad_buf_count(cap_regs_->hcsparams2);
    if (spb > 0) {
        auto arr = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(spb) * 8);
        if (!arr.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        scratchpad_arr_buf_ = std::move(arr.value());
        auto pages = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(spb) * kPageSize);
        if (!pages.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        scratchpad_pages_buf_ = std::move(pages.value());
        auto*          arrv   = static_cast<uint64_t*>(scratchpad_arr_buf_.virt());
        const uint64_t pbase  = scratchpad_pages_buf_.phys();
        for (uint32_t i = 0; i < spb; ++i) {
            arrv[i] = pbase + static_cast<uint64_t>(i) * kPageSize;
        }
        static_cast<uint64_t*>(dcbaa_buf_.virt())[0] = scratchpad_arr_buf_.phys();
    }

    // 3. Command ring (CRCR).
    {
        auto b =
            cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kCommandRingTrbs + 1) * 16);
        if (!b.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        cmd_ring_buf_ = std::move(b.value());
        zero_bytes(cmd_ring_buf_.virt(), static_cast<uint64_t>(kCommandRingTrbs + 1) * 16);
        cmd_ring_.init(static_cast<Trb*>(cmd_ring_buf_.virt()), kCommandRingTrbs,
                       cmd_ring_buf_.phys());
        const uint64_t crcr = cmd_ring_buf_.phys() | Crcr::kRingCycleState;
        op_regs_->crcr_lo   = static_cast<uint32_t>(crcr);
        op_regs_->crcr_hi   = static_cast<uint32_t>(crcr >> 32);
    }

    // 4. Event ring + one-segment ERST + IR0 enable.
    {
        auto b = cinux::drivers::dma::g_dma_pool.alloc(static_cast<uint64_t>(kEventRingTrbs) * 16);
        if (!b.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        event_ring_buf_ = std::move(b.value());
        zero_bytes(event_ring_buf_.virt(), static_cast<uint64_t>(kEventRingTrbs) * 16);
        event_ring_.init(static_cast<Trb*>(event_ring_buf_.virt()), kEventRingTrbs);

        auto e = cinux::drivers::dma::g_dma_pool.alloc(sizeof(ErstEntry));
        if (!e.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        erst_buf_ = std::move(e.value());
        auto* seg = static_cast<ErstEntry*>(erst_buf_.virt());
        seg->base = event_ring_buf_.phys();
        seg->size = static_cast<uint16_t>(kEventRingTrbs);

        ir0_->erstsz          = 1;  // one segment
        const uint64_t erstba = erst_buf_.phys();
        ir0_->erstba_lo       = static_cast<uint32_t>(erstba);
        ir0_->erstba_hi       = static_cast<uint32_t>(erstba >> 32);
        const uint64_t erdp   = event_ring_buf_.phys();
        ir0_->erdp_lo         = static_cast<uint32_t>(erdp);
        ir0_->erdp_hi         = static_cast<uint32_t>(erdp >> 32);
        ir0_->imod            = 0;  // no moderation (interrupt every event)
    }

    // 4b. MSI-X: arm vector kXhciIrqVector (IDT 0x40) for the event-ring
    //     interrupt, and register the event-ring service hook.  (Live CPU
    //     delivery needs sti+APIC, present in the production kernel -- the
    //     test kernel observes IMAN.IP + polls the event ring instead.)
    msix_cap_ = pci::msix::find_capability(dev_.bus, dev_.slot, dev_.func, &pci::PCI::pci_read);
    if (msix_cap_.found) {
        auto mr = msix_.init(msix_cap_, dev_);
        if (!mr.ok()) {
            return cinux::lib::Error::OutOfMemory;
        }
        msix_.mask_all();
        msix_.program_vector(0, kXhciIrqVector, 0);  // entry 0 -> vector 0x40, BSP
        msix_.enable();
    }
    s_instance_ = this;
    set_xhci_irq_hook(&event_irq_thunk);

    // 5. CONFIG.MaxSlotsEn.
    op_regs_->config = (op_regs_->config & ~Config::kMaxSlotsEnMask) | max_slots_;

    // 6. Run: USBCMD = RS | INTE, wait for HCH to clear (running).
    op_regs_->usbcmd = Usbcmd::kRunStop | Usbcmd::kIntEnable;
    for (uint32_t i = 0; i < kResetIters; ++i) {
        if (!(op_regs_->usbsts & Usbsts::kHcHalted)) {
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    if (op_regs_->usbsts & Usbsts::kHcHalted) {
        cinux::lib::kprintf("[xHCI] start failed: controller did not leave HCH\n");
        return cinux::lib::Error::TimedOut;
    }

    // Enable interrupter 0.  This is spec-correct and arms MSI-X event delivery
    // on real hardware.  Under QEMU + nested-KVM IMAN.IE does not reliably latch,
    // so the production event-service path polls the ring from the gui_worker
    // (see init.cpp); the MSI-X setup is left in place for real HW / future QEMU.
    // Drain boot-time events first so IP is clear when IE is written.
    poll_events();
    ir0_->iman = (ir0_->iman & ~Iman::kPending) | Iman::kEnable;

    cinux::lib::kprintf("[xHCI] running (MaxSlotsEn=%u, scratchpad=%u, event ring armed)\n",
                        max_slots_, spb);
    return {};
}

void XHCIController::submit_command(uint64_t parameter, uint32_t status, uint32_t control) {
    cmd_ring_.enqueue(parameter, status, control);
    doorbells_[0] = Doorbell::kTargetCommandRing;  // ring doorbell 0, target 0 (command ring)
}

void XHCIController::poll_events() {
    Trb ev;
    while (event_ring_.dequeue(ev)) {
        const uint32_t type = trb_type(ev.control);
        if (type == TrbType::kCommandCompletion) {
            ++cmd_completion_count_;
            last_cmd_completion_ = ev;  // most recent CCE (run_command matches it)
        } else if (type == TrbType::kTransferEvent) {
            last_transfer_event_ = ev;  // most recent Transfer Event (run_transfer)
            // Async dispatch (Batch 5A): hand the event to the listener
            // registered for this slot (e.g. UsbMouse/UsbKeyboard).  The
            // controller stays a generic transport layer -- it does NOT decode
            // the payload; the device driver does, inside on_transfer_complete.
            // No registered listener -> the synchronous run_transfer path still
            // matches last_transfer_event_ above.
            const uint8_t slot   = static_cast<uint8_t>(cmd_completion_slot_id(ev.control));
            if (slot < kListenerSlots && by_slot_[slot] != nullptr) {
                const uint8_t epid = static_cast<uint8_t>(transfer_event_epid(ev.control));
                by_slot_[slot]->on_transfer_complete(slot, epid, ev);
            }
        }
    }
    // Advance ERDP to the current dequeue pointer (acknowledges processed events
    // to the controller; the 16-byte-aligned ptr clears EHB).
    const uint64_t erdp =
        event_ring_buf_.phys() + static_cast<uint64_t>(event_ring_.dequeue_index()) * 16;
    ir0_->erdp_lo = static_cast<uint32_t>(erdp);
    ir0_->erdp_hi = static_cast<uint32_t>(erdp >> 32);
    // Clear IMAN.IP (write-1-to-clear).
    ir0_->iman    = ir0_->iman | Iman::kPending;
}

cinux::lib::ErrorOr<Trb> XHCIController::run_command(uint64_t parameter, uint32_t status,
                                                     uint32_t control) {
    // Capture the command-TRB slot we are about to fill, so we can match the
    // Command Completion Event whose parameter echoes its physical address.
    const uint64_t expected =
        cmd_ring_buf_.phys() + static_cast<uint64_t>(cmd_ring_.enqueue_index()) * 16;
    submit_command(parameter, status, control);
    for (uint32_t i = 0; i < kResetIters; ++i) {
        poll_events();
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();  // Batch 5A fix: yield while busy-polling so the
                                              // scheduler can run the desktop/gui_worker
        }
        if (trb_type(last_cmd_completion_.control) == TrbType::kCommandCompletion &&
            (last_cmd_completion_.parameter & 0xFFFFFFFFFFFFFFC0ULL) ==
                (expected & 0xFFFFFFFFFFFFFFC0ULL)) {
            return last_cmd_completion_;
        }
    }
    cinux::lib::kprintf("[xHCI] run_command timed out (param=0x%lx)\n",
                        static_cast<unsigned long>(parameter));
    return cinux::lib::Error::TimedOut;
}

cinux::lib::ErrorOr<Trb> XHCIController::run_transfer(uint8_t slot_id, uint8_t epid) {
    // Ring the slot's EP doorbell: target [7:0] = endpoint DCI (1 = EP0), stream 0.
    doorbells_[slot_id] = epid;
    for (uint32_t i = 0; i < kResetIters; ++i) {
        poll_events();
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
        if (trb_type(last_transfer_event_.control) == TrbType::kTransferEvent &&
            cmd_completion_slot_id(last_transfer_event_.control) == slot_id &&
            transfer_event_epid(last_transfer_event_.control) == epid) {
            return last_transfer_event_;
        }
    }
    cinux::lib::kprintf("[xHCI] run_transfer timed out (slot=%u epid=%u)\n",
                        static_cast<unsigned>(slot_id), static_cast<unsigned>(epid));
    return cinux::lib::Error::TimedOut;
}

volatile uint32_t* XHCIController::portsc(uint8_t port) {
    return reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(op_regs_) +
                                                PortRegs::kBaseOffset +
                                                static_cast<uint64_t>(port) * PortRegs::kSpacing);
}

uint32_t XHCIController::read_portsc(uint8_t port) {
    return *portsc(port);
}

cinux::lib::ErrorOr<uint32_t> XHCIController::port_reset(uint8_t port) {
    volatile uint32_t* psc = portsc(port);
    // Power the port (no-op on QEMU qemu-xhci: PPC=0, ports always powered;
    // real hardware with HCC PPC needs this before reset).
    *psc                   = *psc | Portsc::kPortPower;
    // Assert reset; the controller clears PORT_RESET when reset completes.
    *psc                   = *psc | Portsc::kPortReset;
    for (uint32_t i = 0; i < kResetIters; ++i) {
        if (!(*psc & Portsc::kPortReset)) {
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    if (*psc & Portsc::kPortReset) {
        cinux::lib::kprintf("[xHCI] port %u reset did not complete (PORTSC=0x%x)\n",
                            static_cast<unsigned>(port), *psc);
        return cinux::lib::Error::TimedOut;
    }
    // Clear the change bits (write-1-to-clear) so they don't linger.
    *psc = *psc | (Portsc::kConnectStatusChange | Portsc::kPortEnableChange |
                   Portsc::kPortResetChange | Portsc::kPortLinkStateChange);
    return Portsc::portsc_speed(*psc);
}

void XHCIController::dcbaa_set(uint8_t slot, uint64_t phys) {
    static_cast<uint64_t*>(dcbaa_buf_.virt())[slot] = phys;
}

void XHCIController::event_irq_thunk() {
    if (s_instance_ != nullptr) {
        s_instance_->poll_events();
    }
}

void XHCIController::register_transfer_listener(uint8_t slot_id, TransferListener* listener) {
    if (slot_id < kListenerSlots) {
        by_slot_[slot_id] = listener;
    }
}

void XHCIController::ring_doorbell(uint8_t slot_id, uint8_t epid) {
    doorbells_[slot_id] = epid;  // target [7:0] = endpoint DCI, stream 0
}

}  // namespace cinux::drivers::usb
