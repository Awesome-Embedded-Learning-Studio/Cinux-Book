/**
 * @file    ahci_identify.cpp
 * @brief   The identify-word readers: pure arithmetic on drive data.
 *
 * @author  Charliechen114514
 * @date    2026-10-09
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/ahci/ahci_identify.hpp"

#include <stdint.h>

#include <array>

#include "cinux/bit_ops/bit_ops.hpp"
#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

namespace {

/// @brief Sector-ceiling word at the low end of the address.
constexpr cinux::base::bit::BitRange kCeilingLow{.low = 0, .width = 16};

/// @brief Second ceiling word, counting up.
constexpr cinux::base::bit::BitRange kCeilingMid{.low = 16, .width = 16};

/// @brief Third ceiling word, counting up.
constexpr cinux::base::bit::BitRange kCeilingHigh{.low = 32, .width = 16};

/// @brief Top ceiling word, the 48-bit address's last stretch.
constexpr cinux::base::bit::BitRange kCeilingTop{.low = 48, .width = 16};

}  // namespace

uint64_t SectorCeilingFromIdentify(const std::array<uint16_t, 256>& words) {
    cinux::base::bit::BitMask<uint64_t> ceiling{};
    ceiling.deposit(kCeilingLow, words[100]);
    ceiling.deposit(kCeilingMid, words[101]);
    ceiling.deposit(kCeilingHigh, words[102]);
    ceiling.deposit(kCeilingTop, words[103]);
    return ceiling.raw;
}

void ModelFromIdentify(const std::array<uint16_t, 256>& words, std::array<char, 41>& model) {
    for (unsigned long index = 0; index < 20; ++index) {
        const uint16_t kPair   = words[27 + index];
        model[(2 * index)]     = static_cast<char>(cinux::base::bit::HighByte(kPair));
        model[(2 * index) + 1] = static_cast<char>(cinux::base::bit::LowByte(kPair));
    }
    model[40] = '\0';
}

namespace {

/// @brief Word 0 bit 15: set on CFA drives, which this driver does not speak.
constexpr uint16_t kWord0Cfa = 0x8000;

/// @brief Word 0 bit 2: the identify data is incomplete (PUIS staged).
constexpr uint16_t kWord0Incomplete = 0x0004;

/// @brief Word 83 validity pair: bit 15 zero and bit 14 one means the word counts.
constexpr uint16_t kWord83Valid   = 0x4000;
constexpr uint16_t kWord83Invalid = 0xC000;

/// @brief Word 83 bit 10: the drive answers 48-bit addressing.
constexpr uint16_t kWord83Lba48 = 0x0400;

/// @brief Word 255 low byte's marker for a checksum the drive actually wrote.
constexpr uint16_t kChecksumMark = 0x00A5;

/// @brief The encoding ceiling of the 48-bit sector count.
constexpr uint64_t kSectorCeilingTop = 0x0000FFFFFFFFFFFFULL;

bool model_is_printable_and_nonblank(const std::array<uint16_t, 256>& words) {
    std::array<char, 41> model{};
    ModelFromIdentify(words, model);
    bool saw_print = false;
    for (unsigned long index = 0; index < 40; ++index) {
        const unsigned kGlyph = static_cast<unsigned char>(model[index]);
        if (kGlyph == 0x20) {
            continue;
        }
        if (kGlyph < 0x20 || kGlyph > 0x7E) {
            return false;
        }
        saw_print = true;
    }
    return saw_print;
}

bool checksum_holds_when_marked(const std::array<uint16_t, 256>& words) {
    if (cinux::base::bit::LowByte(words[255]) != kChecksumMark) {
        return true;
    }
    unsigned sum = 0;
    for (const uint16_t kWord : words) {
        sum = (sum + cinux::base::bit::LowByte(kWord) + cinux::base::bit::HighByte(kWord)) & 0xFF;
    }
    return sum == 0;
}

}  // namespace

bool IdentifyAdmits(const std::array<uint16_t, 256>& words) {
    if ((words[0] & kWord0Cfa) != 0 || (words[0] & kWord0Incomplete) != 0) {
        return false;
    }
    if (!model_is_printable_and_nonblank(words)) {
        return false;
    }
    if ((words[83] & kWord83Invalid) != kWord83Valid || (words[83] & kWord83Lba48) == 0) {
        return false;
    }
    const uint64_t kSectors = SectorCeilingFromIdentify(words);
    if (kSectors < 1 || kSectors > kSectorCeilingTop) {
        return false;
    }
    return checksum_holds_when_marked(words);
}

}  // namespace cinux::driver
