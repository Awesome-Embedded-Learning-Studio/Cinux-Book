/**
 * @file    ahci_layout.cpp
 * @brief   The AHCI command-word encoders: pure packing, no registers.
 *
 * Bit positions are the only thing hand-written; every mask derives from
 * a position through the BitMask vocabulary. One TU both worlds compile,
 * so host tests check the packing without a controller in sight.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/ahci/ahci_layout.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

namespace {

/// @brief Command header field: FIS length counted in dwords.
constexpr cinux::base::bit::BitRange kHeaderFisLength{.low = 0, .width = 5};

/// @brief Command header field: scatter-gather leg count, the high half of DW0.
constexpr cinux::base::bit::BitRange kHeaderPrdtLength{.low = 16, .width = 16};

/// @brief Command header bit: the command writes to the device.
constexpr AhciReg kHeaderWrite = cinux::base::bit::MaskBit<uint32_t>(6);

/// @brief Region field: bytes in the span, minus one per the spec.
constexpr cinux::base::bit::BitRange kRegionByteCount{.low = 0, .width = 22};

/// @brief Region bit: interrupt when this leg completes.
constexpr AhciReg kRegionInterrupt = cinux::base::bit::MaskBit<uint32_t>(31);

}  // namespace

void FillRegisterFis(RegisterFis& fis, uint32_t command, uint64_t lba, uint16_t count) {
    fis         = RegisterFis{};
    fis.type    = kFisRegisterHostToDevice;
    fis.flags   = kFisFlagCommand;
    fis.command = static_cast<uint8_t>(command);
    fis.device  = kAtaDeviceLba;
    fis.count   = count;
    const cinux::base::bit::BitMask<uint64_t> kAddress{lba};
    for (unsigned long byte = 0; byte < 3; ++byte) {
        const cinux::base::bit::BitRange kLowByte{.low   = static_cast<unsigned char>(8 * byte),
                                                  .width = 8};
        const cinux::base::bit::BitRange kHighByte{
            .low = static_cast<unsigned char>(24 + (8 * byte)), .width = 8};
        fis.lba_low[byte]  = static_cast<uint8_t>(kAddress.extract(kLowByte));
        fis.lba_high[byte] = static_cast<uint8_t>(kAddress.extract(kHighByte));
    }
}

uint32_t MakeHeaderFlags(bool write, unsigned long legs) {
    AhciReg flags{};
    flags.deposit(kHeaderFisLength, 5);
    flags.deposit(kHeaderPrdtLength, legs);
    return (flags | (write ? kHeaderWrite : AhciReg{})).raw;
}

uint32_t MakeRegionFlags(unsigned long bytes) {
    AhciReg flags{};
    flags.deposit(kRegionByteCount, bytes - 1);
    return (flags | kRegionInterrupt).raw;
}

}  // namespace cinux::driver
