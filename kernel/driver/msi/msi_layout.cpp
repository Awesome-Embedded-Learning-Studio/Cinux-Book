/**
 * @file    msi_layout.cpp
 * @brief   The MSI and MSI-X decoders: pure reads, no ports.
 *
 * Field positions are the only thing hand-written against the bit
 * vocabulary. One TU both worlds compile, so the host tests decode
 * shaped headers without a bus in sight.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/msi/msi_layout.hpp"

#include <stdint.h>

#include <array>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/driver/pci/pci_layout.hpp"

namespace cinux::driver::msi {

namespace {

/// @brief One configuration-space dword seen through the bit vocabulary.
using ConfigWord = cinux::base::bit::BitMask<uint32_t>;

/// @brief MSI control field in the header dword: signaling is enabled.
constexpr cinux::base::bit::BitRange kMsiEnabled{.low = 16, .width = 1};

/// @brief MSI control field: how many vectors may be enabled, log2.
constexpr cinux::base::bit::BitRange kMsiVectorCap{.low = 20, .width = 3};

/// @brief MSI control field: message addresses stretch to 64 bits.
constexpr cinux::base::bit::BitRange kMsiWideAddress{.low = 23, .width = 1};

/// @brief MSI control field: per-vector masking is supported.
constexpr cinux::base::bit::BitRange kMsiMaskable{.low = 24, .width = 1};

/// @brief Data field of the dword an MSI data word lives in.
constexpr cinux::base::bit::BitRange kMsiData{.low = 0, .width = 16};

/// @brief MSI-X control field: how many table entries, minus one.
constexpr cinux::base::bit::BitRange kMsixTableSize{.low = 16, .width = 11};

/// @brief MSI-X control field: the function-wide mask, as kept.
constexpr cinux::base::bit::BitRange kMsixMasked{.low = 30, .width = 1};

/// @brief MSI-X control field: message signaling is enabled.
constexpr cinux::base::bit::BitRange kMsixEnabled{.low = 31, .width = 1};

/// @brief Window field: where a table starts inside its base register.
constexpr cinux::base::bit::BitRange kWindowOffset{.low = 3, .width = 29};

/// @brief Window field: which base register holds a table.
constexpr cinux::base::bit::BitRange kWindowBar{.low = 0, .width = 3};

/// @brief The dword that follows a capability header.
uint8_t after_header(uint8_t offset, unsigned step) {
    return static_cast<uint8_t>(offset + step);
}

}  // namespace

bool DecodeMsi(PciConfigRead read, const void* context, PciSpot spot, MsiFacts& into) {
    const uint8_t kCap = FindCapability(read, context, spot, kMsiCapId);
    if (kCap == 0) {
        return false;
    }
    const ConfigWord kHead{read(context, spot, kCap)};
    into              = MsiFacts{};
    into.enabled      = kHead.extract(kMsiEnabled) != 0;
    into.wide_address = kHead.extract(kMsiWideAddress) != 0;
    into.maskable     = kHead.extract(kMsiMaskable) != 0;
    into.vector_cap   = static_cast<uint8_t>(kHead.extract(kMsiVectorCap));
    into.address      = read(context, spot, after_header(kCap, 4));
    if (into.wide_address) {
        into.address_high = read(context, spot, after_header(kCap, 8));
        into.data         = static_cast<uint16_t>(
            ConfigWord{read(context, spot, after_header(kCap, 12))}.extract(kMsiData));
        return true;
    }
    into.data = static_cast<uint16_t>(
        ConfigWord{read(context, spot, after_header(kCap, 8))}.extract(kMsiData));
    return true;
}

bool DecodeMsix(PciConfigRead read, const void* context, PciSpot spot, MsixFacts& into) {
    const uint8_t kCap = FindCapability(read, context, spot, kMsixCapId);
    if (kCap == 0) {
        return false;
    }
    const ConfigWord kHead{read(context, spot, kCap)};
    const ConfigWord kTable{read(context, spot, after_header(kCap, 4))};
    const ConfigWord kPending{read(context, spot, after_header(kCap, 8))};
    into                = MsixFacts{};
    into.table_size     = static_cast<uint16_t>(kHead.extract(kMsixTableSize) + 1);
    into.enabled        = kHead.extract(kMsixEnabled) != 0;
    into.masked         = kHead.extract(kMsixMasked) != 0;
    into.table_bir      = static_cast<uint8_t>(kTable.extract(kWindowBar));
    into.table_offset   = static_cast<uint32_t>(kTable.extract(kWindowOffset));
    into.pending_bir    = static_cast<uint8_t>(kPending.extract(kWindowBar));
    into.pending_offset = static_cast<uint32_t>(kPending.extract(kWindowOffset));
    return true;
}

MsixWindows ResolveMsixWindows(const MsixFacts& facts, const std::array<uint32_t, 6>& bars) {
    return {
        .table = BarBase(bars, static_cast<unsigned long>(facts.table_bir)) + facts.table_offset,
        .pending =
            BarBase(bars, static_cast<unsigned long>(facts.pending_bir)) + facts.pending_offset,
    };
}

}  // namespace cinux::driver::msi
