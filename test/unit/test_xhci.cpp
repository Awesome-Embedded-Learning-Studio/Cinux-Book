/**
 * @file test/unit/test_xhci.cpp
 * @brief Host unit tests for xHCI register layout + pure helpers
 *
 * Verifies the packed MMIO struct sizes (static_asserts in the header fire on
 * include) and the HCSPARAMS2 scratchpad decode.  TRB ring math lands here in
 * Batch 2A.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

#    include "drivers/usb/xhci_context.hpp"
#    include "drivers/usb/xhci_registers.hpp"
#    include "drivers/usb/xhci_ring.hpp"

// TrbType is a namespace, which a using-declaration cannot name (GCC) -- use a
// using-directive for the usb namespace instead (same GOTCHA as test_msix).
using namespace cinux::drivers::usb;

// ============================================================
// 1. Packed MMIO struct sizes (match the xHCI spec offsets)
// ============================================================

TEST("xhci: XhciCapRegs is 0x20 bytes") {
    ASSERT_EQ(sizeof(XhciCapRegs), 0x20u);
}

TEST("xhci: XhciOpRegs is 0x3C bytes") {
    ASSERT_EQ(sizeof(XhciOpRegs), 0x3Cu);
}

TEST("xhci: XhciInterrupterRegs is 0x20 bytes") {
    ASSERT_EQ(sizeof(XhciInterrupterRegs), 0x20u);
}

// ============================================================
// 2. HCSPARAMS2 scratchpad decode
// ============================================================

TEST("xhci: scratchpad count 0 when both lo/hi zero") {
    ASSERT_EQ(scratchpad_buf_count(0x00000000), 0u);
}

TEST("xhci: scratchpad count uses lo field only when hi zero") {
    // lo = 3, hi = 0 -> 3
    ASSERT_EQ(scratchpad_buf_count(0x00000003), 3u);
}

TEST("xhci: scratchpad count combines lo | (hi << 5)") {
    // lo = 1, hi = 2 -> 1 | (2 << 5) = 1 | 64 = 65
    ASSERT_EQ(scratchpad_buf_count(0x00020001), 65u);
}

// ============================================================
// 3. TrbRing producer: cycle bit + Link-TRB wrap + PCS flip (Batch 2A)
// ============================================================

TEST("xhci: TrbRing enqueue sets cycle = PCS and records the type") {
    Trb     storage[5];  // 4 usable + 1 Link
    TrbRing ring;
    ring.init(storage, 4, 0x1000);

    ASSERT_TRUE(ring.producer_cycle());  // PCS starts true
    ring.enqueue(0xAA, 0, trb_control(TrbType::kNormal));
    ASSERT_EQ(static_cast<uint32_t>(storage[0].control & kCycleBit), kCycleBit);
    ASSERT_EQ(trb_type(storage[0].control), TrbType::kNormal);
    ASSERT_EQ(static_cast<uint64_t>(storage[0].parameter), 0xAAULL);
}

TEST("xhci: TrbRing wraps via Link TRB and flips PCS") {
    Trb     storage[5];  // 4 usable + Link at [4]
    TrbRing ring;
    ring.init(storage, 4, 0x1000);

    for (uint32_t i = 0; i < 4; ++i) {
        ring.enqueue(i, 0, trb_control(TrbType::kNoOp));
    }
    // After 4 enqueues the producer hit slots_=4 -> Link written, wrap, PCS flip.
    ASSERT_EQ(ring.enqueue_index(), 0u);
    ASSERT_FALSE(ring.producer_cycle());
    ASSERT_EQ(trb_type(storage[4].control), TrbType::kLink);
    ASSERT_TRUE(storage[4].control & kToggleCycle);
    ASSERT_EQ(static_cast<uint64_t>(storage[4].parameter), 0x1000ULL);  // base
}

TEST("xhci: TrbRing second lap uses the flipped cycle bit") {
    Trb     storage[5];
    TrbRing ring;
    ring.init(storage, 4, 0x1000);
    for (uint32_t i = 0; i < 4; ++i) {
        ring.enqueue(i, 0, trb_control(TrbType::kNoOp));  // lap 1
    }
    ring.enqueue(0xBB, 0, trb_control(TrbType::kNormal));  // lap 2, first TRB
    ASSERT_FALSE(storage[0].control & kCycleBit);          // cycle = PCS = 0 (flipped)
    ASSERT_EQ(static_cast<uint64_t>(storage[0].parameter), 0xBBULL);
}

// ============================================================
// 4. EventRing consumer: cycle match + CCS flip on wrap (Batch 2A)
// ============================================================

TEST("xhci: EventRing dequeues events while cycle matches CCS") {
    Trb       storage[4];
    EventRing ring;
    ring.init(storage, 4);
    // Controller wrote two events with cycle = PCS = 1; [2] still empty (cycle 0).
    storage[0].control = trb_control(TrbType::kCommandCompletion) | kCycleBit;
    storage[1].control = trb_control(TrbType::kCommandCompletion) | kCycleBit;
    storage[2].control = 0;

    Trb ev;
    ASSERT_TRUE(ring.dequeue(ev));
    ASSERT_EQ(trb_type(ev.control), TrbType::kCommandCompletion);
    ASSERT_TRUE(ring.dequeue(ev));
    ASSERT_FALSE(ring.dequeue(ev));  // empty
}

TEST("xhci: EventRing flips CCS when the dequeue pointer wraps") {
    Trb       storage[2];  // size 2
    EventRing ring;
    ring.init(storage, 2);
    // Controller fills both (cycle 1), wraps, flips to cycle 0, rewrites [0].
    storage[0].control = trb_control(TrbType::kTransferEvent) | kCycleBit;
    storage[1].control = trb_control(TrbType::kTransferEvent) | kCycleBit;

    Trb ev;
    ASSERT_TRUE(ring.dequeue(ev));
    ASSERT_EQ(ring.dequeue_index(), 1u);
    ASSERT_TRUE(ring.dequeue(ev));  // wraps 1->2->0, CCS flips to false
    ASSERT_EQ(ring.dequeue_index(), 0u);
    ASSERT_FALSE(ring.consumer_cycle());

    // New lap: controller writes [0] with cycle 0 (matches new CCS).
    storage[0].control = trb_control(TrbType::kTransferEvent);  // cycle 0
    ASSERT_TRUE(ring.dequeue(ev));
}

// ============================================================
// 5. xHCI context field encoders (Batch 3A) -- verified bit positions
// ============================================================

TEST("xhci: SlotContext / EndpointContext / InputControlContext are 32 bytes") {
    ASSERT_EQ(sizeof(SlotContext), 32u);
    ASSERT_EQ(sizeof(EndpointContext), 32u);
    ASSERT_EQ(sizeof(InputControlContext), 32u);
}

TEST("xhci: slot_dev_info packs route[19:0], speed[23:20], last_ctx[31:27]") {
    // route=0x12345, speed=high(3), last_ctx=1 (EP0-only device)
    const uint32_t v = slot_dev_info(0x12345, UsbSpeed::kHigh, 1);
    ASSERT_EQ(v & 0xFFFFF, 0x12345u);  // route
    ASSERT_EQ((v >> 20) & 0xF, 3u);    // speed
    ASSERT_EQ(v >> 27, 1u);            // last_ctx
    ASSERT_FALSE(v & (1U << 25));      // MTT clear
    ASSERT_FALSE(v & (1U << 26));      // Hub clear
}

TEST("xhci: slot_dev_info sets MTT/Hub bits") {
    const uint32_t v = slot_dev_info(0, UsbSpeed::kHigh, 1, true, true);
    ASSERT_TRUE(v & (1U << 25));  // MTT
    ASSERT_TRUE(v & (1U << 26));  // Hub
}

TEST("xhci: slot_dev_info2 packs max_exit[15:0], rh_port[23:16], max_ports[31:24]") {
    const uint32_t v = slot_dev_info2(0x1024, 5, 8);
    ASSERT_EQ(v & 0xFFFF, 0x1024u);
    ASSERT_EQ((v >> 16) & 0xFF, 5u);
    ASSERT_EQ((v >> 24) & 0xFF, 8u);
}

TEST("xhci: slot_dev_state packs dev_addr[7:0], slot_state[31:27]") {
    const uint32_t v = slot_dev_state(0x2A, SlotState::kAddressed);
    ASSERT_EQ(v & 0xFF, 0x2Au);
    ASSERT_EQ(v >> 27, 2u);  // addressed
}

TEST("xhci: ep_info2 packs cerr[2:1], ep_type[6:3], max_packet[31:16]") {
    // control EP (4), max packet 64, cerr default 3
    const uint32_t v = ep_info2(EpType::kControl, 64);
    ASSERT_EQ((v >> 1) & 0x3, 3u);  // cerr
    ASSERT_EQ((v >> 3) & 0x7, 4u);  // ep_type = control
    ASSERT_EQ(v >> 16, 64u);        // max_packet
}

TEST("xhci: ep_info packs ep_state[2:0] and interval[23:16]") {
    const uint32_t v = ep_info(EpState::kRunning, 0x0A);
    ASSERT_EQ(v & 0x7, 1u);  // running
    ASSERT_EQ((v >> 16) & 0xFF, 0xAu);
}

TEST("xhci: ep_dequeue_ptr keeps phys in [63:4] and sets DCS bit 0") {
    const uint64_t v = ep_dequeue_ptr(0x1000, true);
    ASSERT_EQ(v >> 4, 0x100ULL);  // phys / 16
    ASSERT_EQ(v & 1ULL, 1ULL);    // DCS
    // unaligned low nibble is masked off, DCS still set
    ASSERT_EQ(ep_dequeue_ptr(0x1003, true) >> 4, 0x100ULL);
    ASSERT_EQ(ep_dequeue_ptr(0x1000, false) & 1ULL, 0ULL);  // dcs=false clears bit 0
}

TEST("xhci: ep_tx_info packs avg_trb_len[15:0], max_esit_payload[31:16]") {
    const uint32_t v = ep_tx_info(0x1234, 0x00FF);
    ASSERT_EQ(v & 0xFFFF, 0x1234u);
    ASSERT_EQ(v >> 16, 0xFFu);
}

TEST("xhci: input_add_flag sets context-index bit (slot=bit0, EP0=bit1)") {
    ASSERT_EQ(input_add_flag(0), 1u);  // slot
    ASSERT_EQ(input_add_flag(1), 2u);  // EP0
    ASSERT_EQ(input_add_flag(3), 8u);  // EP1-in
}

// ============================================================
// 6. PORTSC + Command Completion Event parsing (Batch 3B)
// ============================================================

TEST("xhci: portsc_speed extracts device speed [13:10]") {
    ASSERT_EQ(Portsc::portsc_speed(1u << 10), 1u);  // full speed
    ASSERT_EQ(Portsc::portsc_speed(3u << 10), 3u);  // high speed
    ASSERT_EQ(Portsc::portsc_speed(0), 0u);         // undefined
}

TEST("xhci: portsc_link_state extracts [8:5]") {
    ASSERT_EQ(Portsc::portsc_link_state(7u << 5), 7u);  // polling
    ASSERT_EQ(Portsc::portsc_link_state(0), 0u);        // U0
}

TEST("xhci: cmd_completion_slot_id extracts [31:24] of control") {
    ASSERT_EQ(cmd_completion_slot_id(5u << 24), 5u);
    ASSERT_EQ(cmd_completion_slot_id(0), 0u);
}

TEST("xhci: cmd_completion_code extracts [31:24] of status") {
    ASSERT_EQ(cmd_completion_code(1u << 24), CompCode::kSuccess);
    ASSERT_EQ(cmd_completion_code(5u << 24), 5u);  // TRB error
}

TEST("xhci: slot_id_for_trb packs slot into [31:24]") {
    ASSERT_EQ(slot_id_for_trb(3), 3u << 24);
}

TEST("xhci: Address Device TRB control carries type + slot id") {
    const uint32_t ctrl = trb_control(TrbType::kAddressDevice) | slot_id_for_trb(2);
    ASSERT_EQ(trb_type(ctrl), TrbType::kAddressDevice);
    ASSERT_EQ(cmd_completion_slot_id(ctrl), 2u);
}

// ============================================================
// 7. Control-transfer stage TRB builders (Batch 3C) -- verified control words
// (TRT at control [17:16], IDT bit6, CHAIN bit4, IOC bit5, DIR bit16, ISP bit2)
// ============================================================

TEST("xhci: setup_stage_control packs IDT+CHAIN+type + TRT at [17:16]") {
    // IN data (GET_DESCRIPTOR): type=2, IDT(bit6), CHAIN(bit4), TRT=3<<16 -> 0x00030850
    ASSERT_EQ(setup_stage_control(Trt::kIn), 0x00030850u);
    // no data (SET_CONFIGURATION): TRT=0 -> 0x00000850
    ASSERT_EQ(setup_stage_control(Trt::kNone), 0x00000850u);
    // OUT data: TRT=2 -> 0x00020850
    ASSERT_EQ(setup_stage_control(Trt::kOut), 0x00020850u);
}

TEST("xhci: data_stage_control sets DIR+ISP for IN, neither for OUT") {
    ASSERT_EQ(data_stage_control(true), 0x00010C14u);   // type3(0xC00) | DIR | ISP | CHAIN
    ASSERT_EQ(data_stage_control(false), 0x00000C10u);  // type3 | CHAIN
}

TEST("xhci: status_stage_control direction is opposite of data stage") {
    // data IN -> status OUT handshake (no DIR): type4 | IOC
    ASSERT_EQ(status_stage_control(true), 0x00001020u);
    // no/OUT data -> status IN handshake (DIR): type4 | IOC | DIR
    ASSERT_EQ(status_stage_control(false), 0x00011020u);
}

TEST("xhci: transfer_event_epid extracts [20:16], remaining extracts [23:0]") {
    // control = (slotid<<24)|(epid<<16)|(type<<10)|cycle; status = remaining | (code<<24)
    const uint32_t ctrl   = (5u << 24) | (1u << 16) | (32u << 10);
    const uint32_t status = 46u | (1u << 24);  // code=Success, remaining=46
    ASSERT_EQ(transfer_event_epid(ctrl), 1u);
    ASSERT_EQ(cmd_completion_slot_id(ctrl), 5u);  // slot id reuses the CCE extractor
    ASSERT_EQ(transfer_event_remaining(status), 46u);
    ASSERT_EQ(cmd_completion_code(status), 1u);  // code reuses the CCE extractor
}

// ============================================================
// 8. Interrupt-IN transfer TRB + endpoint DCI (Batch 4A)
// ============================================================

TEST("xhci: interrupt_in_trb_control packs Normal(type1) + IOC + ISP") {
    // (1<<10=0x400) | IOC(bit5=0x20) | ISP(bit2=0x4) = 0x424, no DIR
    ASSERT_EQ(interrupt_in_trb_control(), 0x00000424u);
}

TEST("xhci: ep_dci maps EP number + direction to the doorbell target") {
    ASSERT_EQ(ep_dci(0, true), 1u);   // EP0 control = DCI 1
    ASSERT_EQ(ep_dci(1, true), 3u);   // EP1-IN  = DCI 3
    ASSERT_EQ(ep_dci(1, false), 2u);  // EP1-OUT = DCI 2
    ASSERT_EQ(ep_dci(2, true), 5u);   // EP2-IN  = DCI 5
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
