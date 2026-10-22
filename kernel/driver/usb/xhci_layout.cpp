/**
 * @file    xhci_layout.cpp
 * @brief   The xHCI arithmetic: ring protocol and region decoding.
 *
 * Field positions are the only thing hand-written; every extraction goes
 * through the bit vocabulary. One TU both worlds compile, so the host
 * tests run rings over plain memory without a controller in sight.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/usb/xhci_layout.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"

namespace cinux::driver::usb {

namespace {

/// @brief One register word seen through the bit vocabulary.
using RegWord = cinux::base::bit::BitMask<uint32_t>;

/// @brief The link TRB's toggle-cycle bit, the handshake's turn signal.
constexpr RegWord kToggleCycle = cinux::base::bit::MaskBit<uint32_t>(1);

/// @brief TRB type field of the control word.
constexpr cinux::base::bit::BitRange kTrbType{.low = 10, .width = 6};

/// @brief Completion-code field of the status word.
constexpr cinux::base::bit::BitRange kCompletion{.low = 24, .width = 8};

/// @brief Slot-id field of an event's control word.
constexpr cinux::base::bit::BitRange kSlotId{.low = 24, .width = 8};

/// @brief Max-slots field of HCSPARAMS1.
constexpr cinux::base::bit::BitRange kMaxSlots{.low = 0, .width = 8};

/// @brief Max-ports field of HCSPARAMS1.
constexpr cinux::base::bit::BitRange kMaxPorts{.low = 24, .width = 8};

/// @brief Capability register: operational-region length byte.
constexpr unsigned long kCapLength = 0x00;

/// @brief Capability register: interface version, sixteen bits.
constexpr unsigned long kCapVersion = 0x00;

/// @brief Capability register: slot and port counts.
constexpr unsigned long kCapHcsParams1 = 0x04;

/// @brief Capability register: doorbell array offset.
constexpr unsigned long kCapDbOff = 0x14;

/// @brief Capability register: runtime-region offset.
constexpr unsigned long kCapRtsOff = 0x18;

/// @brief Reads one dword from the controller's first bytes.
uint32_t read_cap(const volatile unsigned char* capability, unsigned long offset) {
    return *reinterpret_cast<const volatile uint32_t*>(  // NOLINT(performance-no-int-to-ptr)
        capability + offset);
}

/// @brief The producer cycle state as the control word spells it.
uint32_t cycle_word(bool cycle) {
    return cycle ? 1U : 0U;
}

/// @brief One control word: the type in its field, the cycle in bit zero.
uint32_t control_word(uint32_t type, bool cycle) {
    return (type << 10U) | cycle_word(cycle);
}

/// @brief Places the link TRB that closes one ring at its wrap point.
void place_link(Trb* ring, unsigned long slots, uint64_t ring_physical, bool cycle) {
    Trb& link      = ring[slots - 1];
    link.parameter = ring_physical;
    link.status    = 0;
    link.control   = control_word(kTrbLink, cycle) | kToggleCycle.raw;
}

/// @brief Wraps one ring: link placed, cursor home, cycle flipped.
void wrap_ring(Trb* ring, unsigned long slots, uint64_t ring_physical, RingState& state) {
    place_link(ring, slots, ring_physical, state.cycle);
    state.index = 0;
    state.cycle = !state.cycle;
}

}  // namespace

void InitRing(Trb* ring, unsigned long slots, uint64_t ring_physical) {
    base::SetBytes(ring, 0, slots * sizeof(Trb));
    place_link(ring, slots, ring_physical, true);
}

// NOLINTNEXTLINE(readability-function-size) fill, advance and wrap name one ring pass
void EnqueueCommand(Trb* ring, unsigned long slots, RingState& state, uint64_t ring_physical,
                    uint64_t parameter, uint32_t status, uint32_t control) {
    Trb& trb      = ring[state.index];
    trb.parameter = parameter;
    trb.status    = status;
    trb.control   = control | cycle_word(state.cycle);
    ++state.index;
    if (state.index == slots - 1) {
        wrap_ring(ring, slots, ring_physical, state);
    }
}

bool EventPending(const Trb& event, bool consumer_cycle) {
    if (cycle_word(consumer_cycle) != (event.control & 1U)) {
        return false;
    }
    return event.parameter != 0 || event.status != 0 || event.control != 0;
}

uint8_t TrbType(const Trb& trb) {
    return static_cast<uint8_t>(RegWord{trb.control}.extract(kTrbType));
}

uint8_t CompletionCode(const Trb& trb) {
    return static_cast<uint8_t>(RegWord{trb.status}.extract(kCompletion));
}

uint8_t SlotIdOf(const Trb& trb) {
    return static_cast<uint8_t>(RegWord{trb.control}.extract(kSlotId));
}

XhciRegions LocateRegions(const volatile unsigned char* capability) {
    const unsigned long kLength =
        RegWord{read_cap(capability, kCapLength)}.extract({.low = 0, .width = 8});
    const uint32_t kDoorbellBase = read_cap(capability, kCapDbOff);
    const uint32_t kRuntimeBase  = read_cap(capability, kCapRtsOff);
    return XhciRegions{
        .operational = kLength,
        .runtime     = RegWord{kRuntimeBase}.extract({.low = 5, .width = 27}) * 32UL,
        .doorbells   = RegWord{kDoorbellBase}.extract({.low = 2, .width = 30}) * 4UL,
    };
}

XhciShape ReadShape(const volatile unsigned char* capability) {
    const uint32_t kVersion      = read_cap(capability, kCapVersion);
    const uint32_t kCapabilities = read_cap(capability, kCapHcsParams1);
    return XhciShape{
        .version   = RegWord{kVersion}.extract({.low = 16, .width = 16}),
        .max_slots = RegWord{kCapabilities}.extract(kMaxSlots),
        .max_ports = RegWord{kCapabilities}.extract(kMaxPorts),
    };
}

namespace {

/// @brief Route-string field of the slot context's first word.
constexpr cinux::base::bit::BitRange kSlotRoute{.low = 0, .width = 20};

/// @brief Speed field of the slot context's first word.
constexpr cinux::base::bit::BitRange kSlotSpeed{.low = 20, .width = 4};

/// @brief Root-hub-port field of the slot context's second word.
constexpr cinux::base::bit::BitRange kSlotPort{.low = 16, .width = 8};

/// @brief Context-entries field of the slot context's first word.
constexpr cinux::base::bit::BitRange kSlotEntries{.low = 27, .width = 5};

/// @brief Endpoint-type field of an endpoint context's second word.
constexpr cinux::base::bit::BitRange kEpType{.low = 3, .width = 3};

/// @brief Error-retries field of an endpoint context's second word.
constexpr cinux::base::bit::BitRange kEpRetries{.low = 0, .width = 2};

/// @brief Packet-ceiling field of an endpoint context's second word.
constexpr cinux::base::bit::BitRange kEpMaxPacket{.low = 16, .width = 16};

/// @brief Average-length field of an endpoint context's fifth word.
constexpr cinux::base::bit::BitRange kEpAverage{.low = 0, .width = 16};

/// @brief Interval field of an endpoint context's first word.
constexpr cinux::base::bit::BitRange kEpInterval{.low = 16, .width = 8};

/// @brief Endpoint type: bidirectional control.
constexpr uint32_t kEpKindControl = 4;

/// @brief Endpoint type: interrupt, device to host.
constexpr uint32_t kEpKindInterruptIn = 7;

/// @brief The plain column of the boot-keyboard usage table.
constexpr char kPlainKeys[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\0\b\t -=[]\\\0;'`,./";

/// @brief The shifted column of the boot-keyboard usage table.
constexpr char kShiftKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\0\b\t _+{}|\0:\"~<>?";

/// @brief First usage code the tables cover, the letter a.
constexpr unsigned long kFirstKeyUsage = 4;

/// @brief How many usages the tables cover.
constexpr unsigned long kKeyUsageCount = sizeof(kPlainKeys) - 1;

static_assert(sizeof(kPlainKeys) == sizeof(kShiftKeys));

/// @brief Both shift modifier bits as one mask.
constexpr uint8_t kShiftModifiers = 0x22;

/// @brief Whether one usage sits inside a report's key slots.
bool usage_in(const unsigned char* report, uint8_t usage) {
    for (unsigned long slot = 2; slot < 8; ++slot) {
        if (report[slot] == usage) {
            return true;
        }
    }
    return false;
}

/// @brief Builds one endpoint context's shared shape.
// NOLINTNEXTLINE(readability-function-size) one straight fill of one context
void build_ep(uint32_t* dwords, uint32_t kind, uint64_t ring, uint16_t max_packet,
              uint8_t interval) {
    base::SetBytes(dwords, 0, 8 * sizeof(uint32_t));
    RegWord first{dwords[0]};
    first.deposit(kEpInterval, interval);
    dwords[0] = first.raw;
    RegWord word{dwords[1]};
    word.deposit(kEpRetries, 3);
    word.deposit(kEpType, kind);
    word.deposit(kEpMaxPacket, max_packet);
    dwords[1] = word.raw;
    dwords[2] = static_cast<uint32_t>((ring & ~0xFULL) | 1ULL);
    dwords[3] = static_cast<uint32_t>(ring >> 32U);
    RegWord tail{dwords[4]};
    tail.deposit(kEpAverage, kind == kEpKindControl ? 8 : max_packet);
    dwords[4] = tail.raw;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-size) the eight wire bytes, one deposit each
uint64_t PackSetup(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    uint64_t packed = type;
    packed |= static_cast<uint64_t>(request) << 8U;
    packed |= static_cast<uint64_t>(value) << 16U;
    packed |= static_cast<uint64_t>(index) << 32U;
    packed |= static_cast<uint64_t>(length) << 48U;
    return packed;
}

bool FindKeyboardInterface(const unsigned char* config, unsigned long bytes, KeyboardEp& into) {
    unsigned long walk        = 0;
    uint8_t       want_config = 0;
    bool          keyboard    = false;
    while (walk + 2 <= bytes) {
        const unsigned long kLength = config[walk];
        const unsigned long kType   = config[walk + 1];
        if (kLength < 2 || walk + kLength > bytes) {
            return false;
        }
        if (kType == 2 && kLength >= 9) {
            want_config = config[walk + 5];
            keyboard    = false;
        } else if (kType == 4 && kLength >= 9 && config[walk + 5] == 3 && config[walk + 7] == 1) {
            keyboard = true;
        } else if (keyboard && kType == 5 && kLength >= 7 && (config[walk + 2] & 0x80U) != 0 &&
                   (config[walk + 3] & 0x03U) == 3) {
            into              = KeyboardEp{};
            into.config_value = want_config;
            into.address      = config[walk + 2];
            into.max_packet =
                static_cast<uint16_t>(static_cast<unsigned>(config[walk + 4]) |
                                      (static_cast<unsigned>(config[walk + 5]) << 8U));
            into.interval = config[walk + 6];
            return true;
        }
        walk += kLength;
    }
    return false;
}

char HidReportKey(const unsigned char* now, const unsigned char* before) {
    const bool kShifted = (now[0] & kShiftModifiers) != 0;
    for (unsigned long slot = 2; slot < 8; ++slot) {
        const unsigned long kUsage = now[slot];
        if (kUsage < kFirstKeyUsage || kUsage >= kFirstKeyUsage + kKeyUsageCount ||
            usage_in(before, static_cast<uint8_t>(kUsage))) {
            continue;
        }
        const char kGlyph = (kShifted ? kShiftKeys : kPlainKeys)[kUsage - kFirstKeyUsage];
        if (kGlyph != 0) {
            return kGlyph;
        }
    }
    return 0;
}

// NOLINTNEXTLINE(readability-function-size) two words, one field each
void BuildSlotContext(uint32_t* dwords, uint32_t route, uint8_t port, uint8_t speed,
                      uint8_t entries) {
    base::SetBytes(dwords, 0, 8 * sizeof(uint32_t));
    RegWord first{dwords[0]};
    first.deposit(kSlotRoute, route);
    first.deposit(kSlotSpeed, speed);
    first.deposit(kSlotEntries, entries);
    dwords[0] = first.raw;
    RegWord second{dwords[1]};
    second.deposit(kSlotPort, port);
    dwords[1] = second.raw;
}

void BuildControlEpContext(uint32_t* dwords, uint64_t ring, uint16_t max_packet) {
    build_ep(dwords, kEpKindControl, ring, max_packet, 0);
}

void BuildInterruptInEpContext(uint32_t* dwords, uint64_t ring, uint16_t max_packet,
                               uint8_t interval) {
    build_ep(dwords, kEpKindInterruptIn, ring, max_packet, interval);
}

void SetAddFlags(uint32_t* control, uint32_t flags) {
    control[1] = flags;
}

}  // namespace cinux::driver::usb
