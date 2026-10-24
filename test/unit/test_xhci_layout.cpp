#include <stdint.h>

#include "../framework/framework.hpp"
#include "kernel/driver/usb/xhci_layout.hpp"
#include "test_assert.hpp"

namespace {

/// @brief One control word with the type in its field, for expectations.
uint32_t typed(uint32_t type, uint32_t extra, bool cycle) {
    return (type << 10U) | extra | (cycle ? 1U : 0U);
}

}  // namespace

TEST("xhci: the pci predicate demands the xhci programming interface") {
    ASSERT_TRUE(cinux::driver::usb::IsXhciController(0x0C, 0x03, 0x30));
    ASSERT_FALSE(cinux::driver::usb::IsXhciController(0x0C, 0x03, 0x00));
    ASSERT_FALSE(cinux::driver::usb::IsXhciController(0x0C, 0x03, 0x10));
    ASSERT_FALSE(cinux::driver::usb::IsXhciController(0x0C, 0x03, 0x20));
    ASSERT_FALSE(cinux::driver::usb::IsXhciController(0x01, 0x03, 0x30));
    ASSERT_FALSE(cinux::driver::usb::IsXhciController(0x0C, 0x06, 0x30));
}

TEST("xhci: a ring opens with zeros and a live link") {
    cinux::driver::usb::Trb ring[8] = {};
    cinux::driver::usb::InitRing(ring, 8, 0x1000ULL);
    ASSERT_EQ(ring[7].parameter, 0x1000ULL);
    ASSERT_EQ(ring[7].control, typed(cinux::driver::usb::kTrbLink, 1U << 1U, true));
    ASSERT_EQ(ring[0].parameter, 0ULL);
    ASSERT_EQ(ring[0].control, 0U);
}

TEST("xhci: enqueue fills in order") {
    cinux::driver::usb::Trb ring[8] = {};
    cinux::driver::usb::InitRing(ring, 8, 0x2000ULL);
    cinux::driver::usb::RingState state{.index = 0, .cycle = true};
    const uint32_t                kSeat =
        cinux::driver::usb::TrbControl(cinux::driver::usb::kTrbEnableSlot, 1U << 24U);
    for (unsigned long fill = 0; fill < 6; ++fill) {
        cinux::driver::usb::EnqueueCommand(ring, 8, state, 0x2000ULL, fill, 0, kSeat);
    }
    ASSERT_EQ(state.index, 6UL);
    ASSERT_TRUE(state.cycle);
    ASSERT_EQ(ring[5].parameter, 5ULL);
    ASSERT_EQ(ring[5].control, 0x01002401U);
}

TEST("xhci: the seventh enqueue wraps over the link") {
    cinux::driver::usb::Trb ring[8] = {};
    cinux::driver::usb::InitRing(ring, 8, 0x2000ULL);
    cinux::driver::usb::RingState state{.index = 0, .cycle = true};
    const uint32_t                kSeat =
        cinux::driver::usb::TrbControl(cinux::driver::usb::kTrbEnableSlot, 1U << 24U);
    for (unsigned long fill = 0; fill < 6; ++fill) {
        cinux::driver::usb::EnqueueCommand(ring, 8, state, 0x2000ULL, fill, 0, kSeat);
    }
    cinux::driver::usb::EnqueueCommand(ring, 8, state, 0x2000ULL, 6, 0, kSeat);
    ASSERT_EQ(state.index, 0UL);
    ASSERT_FALSE(state.cycle);
    ASSERT_EQ(ring[6].control, 0x01002401U);
    ASSERT_EQ(ring[7].control, typed(cinux::driver::usb::kTrbLink, 1U << 1U, true));

    cinux::driver::usb::EnqueueCommand(ring, 8, state, 0x2000ULL, 7, 0, kSeat);
    ASSERT_EQ(ring[0].control, 0x01002400U);
}

TEST("xhci: event fields decode from the control and status words") {
    cinux::driver::usb::Trb event{};
    event.status  = 1U << 24U;
    event.control = typed(33U, 0U, true);
    event.control |= 3U << 24U;
    ASSERT_TRUE(cinux::driver::usb::EventPending(event, true));
    ASSERT_FALSE(cinux::driver::usb::EventPending(event, false));
    ASSERT_EQ(static_cast<unsigned>(cinux::driver::usb::TrbType(event)), 33U);
    ASSERT_EQ(static_cast<unsigned>(cinux::driver::usb::CompletionCode(event)), 1U);
    ASSERT_EQ(static_cast<unsigned>(cinux::driver::usb::SlotIdOf(event)), 3U);

    const cinux::driver::usb::Trb kSilent{};
    ASSERT_FALSE(cinux::driver::usb::EventPending(kSilent, true));
}

TEST("xhci: the capability region names the register bases and shape") {
    alignas(uint32_t) unsigned char capability[0x20] = {};
    capability[0]                                    = 0x20;
    capability[2]                                    = 0x00;
    capability[3]                                    = 0x01;
    *reinterpret_cast<uint32_t*>(&capability[4])     = 0x04000020U;
    *reinterpret_cast<uint32_t*>(&capability[0x14])  = 0x00002000U;
    *reinterpret_cast<uint32_t*>(&capability[0x18])  = 0x00003000U;
    const volatile unsigned char* view               = capability;

    const cinux::driver::usb::XhciRegions kRegions = cinux::driver::usb::LocateRegions(view);
    ASSERT_EQ(kRegions.operational, 0x20UL);
    ASSERT_EQ(kRegions.runtime, 0x3000UL);
    ASSERT_EQ(kRegions.doorbells, 0x2000UL);

    const cinux::driver::usb::XhciShape kShape = cinux::driver::usb::ReadShape(view);
    ASSERT_EQ(kShape.version, 0x100U);
    ASSERT_EQ(kShape.max_slots, 32U);
    ASSERT_EQ(kShape.max_ports, 4U);
}

TEST("xhci: capability fields exclude reserved bits and keep version separate") {
    const uint32_t    kCapability[8] = {0x0123A540U, 0x12000081U, 0, 0, 0, 0x2003U, 0x301FU, 0};
    const auto* const kView          = reinterpret_cast<const volatile unsigned char*>(kCapability);

    const auto kRegions = cinux::driver::usb::LocateRegions(kView);
    ASSERT_EQ(kRegions.operational, 0x40UL);
    ASSERT_EQ(kRegions.runtime, 0x3000UL);
    ASSERT_EQ(kRegions.doorbells, 0x2000UL);

    const auto kShape = cinux::driver::usb::ReadShape(kView);
    ASSERT_EQ(kShape.version, 0x0123U);
    ASSERT_EQ(kShape.max_slots, 0x81U);
    ASSERT_EQ(kShape.max_ports, 0x12U);
}

TEST("xhci: a setup packet packs its eight wire bytes") {
    ASSERT_EQ(cinux::driver::usb::PackSetup(0x80, 0x06, 0x0100, 0x0000, 18), 0x0012000001000680ULL);
}

TEST("xhci: the boot-keyboard interface hides inside its configuration") {
    const unsigned char kConfiguration[] = {
        0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32, 0x09, 0x04, 0x00,
        0x00, 0x01, 0x03, 0x01, 0x01, 0x00, 0x09, 0x21, 0x10, 0x01, 0x00, 0x01,
        0x22, 0x19, 0x00, 0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    };
    cinux::driver::usb::KeyboardEp keyboard{};
    ASSERT_TRUE(cinux::driver::usb::FindKeyboardInterface(kConfiguration, sizeof(kConfiguration),
                                                          keyboard));
    ASSERT_EQ(keyboard.config_value, 1U);
    ASSERT_EQ(keyboard.address, 0x81U);
    ASSERT_EQ(keyboard.max_packet, 8U);
    ASSERT_EQ(keyboard.interval, 10U);

    const unsigned char kBarren[] = {0x09, 0x02, 0x09, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32};
    cinux::driver::usb::KeyboardEp nothing{};
    ASSERT_FALSE(cinux::driver::usb::FindKeyboardInterface(kBarren, sizeof(kBarren), nothing));
}

TEST("xhci: hid report edges translate through both table columns") {
    const unsigned char kIdle[8]      = {};
    const unsigned char kLowerA[8]    = {0x00, 0, 0x04, 0, 0, 0, 0, 0};
    const unsigned char kUpperA[8]    = {0x22, 0, 0x04, 0, 0, 0, 0, 0};
    const unsigned char kOne[8]       = {0x00, 0, 0x1E, 0, 0, 0, 0, 0};
    const unsigned char kBangShift[8] = {0x02, 0, 0x1E, 0, 0, 0, 0, 0};
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kLowerA, kIdle), 'a');
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kUpperA, kIdle), 'A');
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kOne, kIdle), '1');
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kBangShift, kIdle), '!');
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kIdle, kLowerA), 0);
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kLowerA, kLowerA), 0);
}

TEST("xhci: slot entries share the route and speed word") {
    uint32_t slot[8] = {};
    cinux::driver::usb::BuildSlotContext(slot, 0, 2, 1, 1);
    ASSERT_EQ(slot[0], 0x08100000U);
    ASSERT_EQ(slot[1], 0x00020000U);

    cinux::driver::usb::BuildSlotContext(slot, 0x12345U, 5, 3, 3);
    ASSERT_EQ(slot[0], 0x18312345U);
    ASSERT_EQ(slot[1], 0x00050000U);
}

TEST("xhci: the control context packs its fields") {
    uint32_t control[8] = {};
    cinux::driver::usb::BuildControlEpContext(control, 0x2000ULL, 8);
    ASSERT_EQ(control[0], 0U);
    ASSERT_EQ(control[1], 0x00080023U);
    ASSERT_EQ(control[2], 0x2001U);
    ASSERT_EQ(control[3], 0U);
    ASSERT_EQ(control[4], 8U);
}

TEST("xhci: the interrupt context and add flags pack their fields") {
    uint32_t interrupt[8] = {};
    cinux::driver::usb::BuildInterruptInEpContext(interrupt, 0x3000ULL, 8, 10);
    ASSERT_EQ(interrupt[0], 0x000A0000U);
    ASSERT_EQ(interrupt[1], 0x0008003BU);
    ASSERT_EQ(interrupt[2], 0x3001U);
    ASSERT_EQ(interrupt[4], 8U);

    uint32_t flags[8] = {};
    cinux::driver::usb::SetAddFlags(flags, 0x3U);
    ASSERT_EQ(flags[0], 0U);
    ASSERT_EQ(flags[1], 3U);
}

TEST("xhci: punctuation keeps its hid usages across the non-us key") {
    const unsigned char kIdle[8]   = {};
    const char          kPlain[]   = {'-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'};
    const char          kShifted[] = {'_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'};
    for (unsigned long index = 0; index < sizeof(kPlain); ++index) {
        unsigned char report[8] = {};
        report[2]               = static_cast<unsigned char>(0x2DU + index);
        ASSERT_EQ(cinux::driver::usb::HidReportKey(report, kIdle), kPlain[index]);
        report[0] = 0x02;
        ASSERT_EQ(cinux::driver::usb::HidReportKey(report, kIdle), kShifted[index]);
        ASSERT_EQ(cinux::driver::usb::HidReportKey(report, report), 0);
    }

    const unsigned char kBeyond[8] = {0, 0, 0x39, 0, 0, 0, 0, 0};
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kBeyond, kIdle), 0);
}

TEST("xhci: both control modifiers produce terminal signal bytes") {
    const unsigned char kIdle[8]{};
    const unsigned char kLeftC[8]        = {0x01, 0, 0x06};
    const unsigned char kRightC[8]       = {0x10, 0, 0x06};
    const unsigned char kShiftC[8]       = {0x03, 0, 0x06};
    const unsigned char kControlZ[8]     = {0x01, 0, 0x1D};
    const unsigned char kControlSlash[8] = {0x01, 0, 0x31};
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kLeftC, kIdle), 3);
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kRightC, kIdle), 3);
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kShiftC, kIdle), 3);
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kControlZ, kIdle), 26);
    ASSERT_EQ(cinux::driver::usb::HidReportKey(kControlSlash, kIdle), 28);
}

int main() {
    return cinux::test::RunAll();
}
