/**
 * @file    pci_layout.cpp
 * @brief   The configuration-space encoders: pure arithmetic, no ports.
 *
 * Bit positions are the only thing hand-written; every mask derives from
 * a position through the BitMask vocabulary. One TU both worlds compile,
 * so the host tests exercise these without a bus in sight.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/pci/pci_layout.hpp"

#include <stdint.h>

#include <array>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

namespace {

/// @brief One configuration-space dword seen through the bit vocabulary.
using ConfigWord = cinux::base::bit::BitMask<uint32_t>;

/// @brief Enable bit: without it the cycle is not a configuration cycle.
constexpr ConfigWord kConfigEnable = cinux::base::bit::MaskBit<uint32_t>(31);

/// @brief Bus number field of the configuration address word.
constexpr cinux::base::bit::BitRange kConfigBus{.low = 16, .width = 8};

/// @brief Device number field of the configuration address word.
constexpr cinux::base::bit::BitRange kConfigSlot{.low = 11, .width = 5};

/// @brief Function field of the configuration address word.
constexpr cinux::base::bit::BitRange kConfigFunction{.low = 8, .width = 3};

/// @brief Dword-index field of the configuration address word.
constexpr cinux::base::bit::BitRange kConfigRegister{.low = 2, .width = 6};

/// @brief Flag bit of a bar: the window answers in port I/O space.
constexpr ConfigWord kBarIoSpace = cinux::base::bit::MaskBit<uint32_t>(0);

/// @brief Flag bit of a memory bar: the CPU may read ahead in this window.
constexpr ConfigWord kBarPrefetchable = cinux::base::bit::MaskBit<uint32_t>(3);

/// @brief Width field of a memory bar: where 32-bit and 64-bit are told apart.
constexpr cinux::base::bit::BitRange kBarWidth{.low = 1, .width = 2};

/// @brief The width field's spelling of a 64-bit window.
constexpr uint32_t kBarWidth64 = 0x2;

/// @brief Flag span of an I/O bar: the io bit plus its reserved neighbour.
constexpr cinux::base::bit::BitRange kBarIoFlags{.low = 0, .width = 2};

/// @brief Flag span of a memory bar: io, reserved width bit pair, prefetch.
constexpr cinux::base::bit::BitRange kBarMemoryFlags{.low = 0, .width = 4};

/// @brief Where a 64-bit bar's high half sits inside the combined address.
constexpr cinux::base::bit::BitRange kBarHighHalf{.low = 32, .width = 32};

/// @brief Register offset: command and status share this dword.
constexpr uint8_t kRegStatus = 0x04;

/// @brief Register offset: the capability list's first pointer lives here.
constexpr uint8_t kRegCapabilityPointer = 0x34;

/// @brief Status bit: the device keeps a capability list.
constexpr ConfigWord kStatusHasCaps = cinux::base::bit::MaskBit<uint32_t>(20);

/// @brief Capability id field of a capability header dword.
constexpr cinux::base::bit::BitRange kCapId{.low = 0, .width = 8};

/// @brief Next-pointer field of a capability header dword.
constexpr cinux::base::bit::BitRange kCapNext{.low = 8, .width = 8};

/// @brief The two reserved low bits every capability pointer keeps clear.
constexpr ConfigWord kPointerReserved = ConfigWord{cinux::base::bit::Ones<uint32_t>(2)};

/// @brief The walk's hard bound, where a circling list gives up.
constexpr unsigned kCapabilityWalkLimit = 48;

/// @brief Keeps a pointer's offset, dropping the reserved low bits.
uint8_t aligned_pointer(uint32_t pointer) {
    return static_cast<uint8_t>((ConfigWord{pointer} & ~kPointerReserved).raw);
}

}  // namespace

uint32_t MakeConfigAddress(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    ConfigWord address{kConfigEnable.raw};
    address.deposit(kConfigBus, bus);
    address.deposit(kConfigSlot, slot);
    address.deposit(kConfigFunction, func);
    address.deposit(kConfigRegister, static_cast<uint32_t>(offset) / 4U);
    return address.raw;
}

PciBar DecodeBar(uint32_t raw) {
    const ConfigWord kWord{raw};
    if (kWord.raw == 0) {
        return {.base = 0, .kind = PciBarKind::kUnused, .prefetchable = false};
    }
    if (kWord.has(kBarIoSpace)) {
        ConfigWord base{kWord};
        base.deposit(kBarIoFlags, 0);
        return {.base = base.raw, .kind = PciBarKind::kIo, .prefetchable = false};
    }
    ConfigWord base{kWord};
    base.deposit(kBarMemoryFlags, 0);
    return {
        .base = base.raw,
        .kind =
            kWord.extract(kBarWidth) == kBarWidth64 ? PciBarKind::kMemory64 : PciBarKind::kMemory32,
        .prefetchable = kWord.has(kBarPrefetchable),
    };
}

uint64_t BarBase(const std::array<uint32_t, 6>& bars, unsigned long index) {
    const PciBar kLow = DecodeBar(bars[index]);
    if (kLow.kind != PciBarKind::kMemory64 || index + 1 >= 6) {
        return kLow.base;
    }
    cinux::base::bit::BitMask<uint64_t> wide{kLow.base};
    wide.deposit(kBarHighHalf, bars[index + 1]);
    return wide.raw;
}

uint8_t FindCapability(PciConfigRead read, const void* context, PciSpot spot, uint8_t wanted) {
    if (!ConfigWord{read(context, spot, kRegStatus)}.has(kStatusHasCaps)) {
        return 0;
    }
    uint8_t offset = aligned_pointer(read(context, spot, kRegCapabilityPointer));
    for (unsigned step = 0; offset != 0 && step < kCapabilityWalkLimit; ++step) {
        const ConfigWord kHeader{read(context, spot, offset)};
        if (static_cast<uint8_t>(kHeader.extract(kCapId)) == wanted) {
            return offset;
        }
        offset = aligned_pointer(kHeader.extract(kCapNext));
    }
    return 0;
}

}  // namespace cinux::driver
