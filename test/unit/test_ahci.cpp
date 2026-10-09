#include <stdint.h>

#include <array>
#include <cstddef>

#include "../framework/framework.hpp"
#include "kernel/driver/ahci/ahci_identify.hpp"
#include "kernel/driver/ahci/ahci_layout.hpp"
#include "test_assert.hpp"

TEST("ahci: a filled identify fis carries type, flag, opcode and one sector") {
    cinux::driver::RegisterFis fis{};
    cinux::driver::FillRegisterFis(fis, cinux::driver::kAtaIdentifyDevice, 0, 1);
    ASSERT_EQ(fis.type, 0x27U);
    ASSERT_EQ(fis.flags, 0x80U);
    ASSERT_EQ(fis.command, 0xECU);
    ASSERT_EQ(fis.device, 0x40U);
    ASSERT_EQ(fis.count, 1U);
}

TEST("ahci: features and padding stay zero in a filled fis") {
    cinux::driver::RegisterFis fis{};
    cinux::driver::FillRegisterFis(fis, cinux::driver::kAtaIdentifyDevice, 0, 1);
    ASSERT_EQ(fis.features_low, 0U);
    ASSERT_EQ(fis.features_high, 0U);
    ASSERT_EQ(fis.icc, 0U);
    ASSERT_EQ(fis.control, 0U);
    ASSERT_EQ(fis.zero[0], 0U);
    ASSERT_EQ(fis.zero[47], 0U);
}

TEST("ahci: a 48-bit address splits around the device byte") {
    cinux::driver::RegisterFis fis{};
    const uint64_t             kLba = 0x0001020304050607ULL;
    cinux::driver::FillRegisterFis(fis, cinux::driver::kAtaReadDmaExt, kLba, 8);
    ASSERT_EQ(fis.lba_low[0], 0x07U);
    ASSERT_EQ(fis.lba_low[1], 0x06U);
    ASSERT_EQ(fis.lba_low[2], 0x05U);
    ASSERT_EQ(fis.lba_high[0], 0x04U);
    ASSERT_EQ(fis.lba_high[1], 0x03U);
    ASSERT_EQ(fis.lba_high[2], 0x02U);
    ASSERT_EQ(fis.count, 8U);
}

TEST("ahci: header flags carry fis length, leg count and the write bit") {
    ASSERT_EQ(cinux::driver::MakeHeaderFlags(false, 1), 0x00010005U);
    ASSERT_EQ(cinux::driver::MakeHeaderFlags(true, 1), 0x00010045U);
}

TEST("ahci: a region keeps its byte count on the last dword") {
    cinux::driver::PhysicalRegion region{};
    region.data_base = 0x00346000;
    region.flags     = cinux::driver::MakeRegionFlags(512);
    ASSERT_EQ(region.flags, 0x800001FFU);
    ASSERT_EQ(region.reserved, 0U);
}

TEST("ahci: the wire layouts pin their field offsets") {
    ASSERT_EQ(offsetof(cinux::driver::PhysicalRegion, flags), 0x0CUL);
    ASSERT_EQ(offsetof(cinux::driver::CommandHeader, prd_byte_count), 0x04UL);
    ASSERT_EQ(offsetof(cinux::driver::RegisterFis, device), 0x07UL);
}

TEST("ahci: region flags count bytes-minus-one and ask for completion") {
    ASSERT_EQ(cinux::driver::MakeRegionFlags(512), 0x800001FFU);
    ASSERT_EQ(cinux::driver::MakeRegionFlags(4096), 0x80000FFFU);
}

TEST("ahci: the sector ceiling reads the four lba-48 words") {
    std::array<uint16_t, 256> words = {};
    words[100]                      = 0xABCD;
    words[101]                      = 0x1234;
    words[102]                      = 0x0001;
    words[103]                      = 0x0000;
    ASSERT_EQ(cinux::driver::SectorCeilingFromIdentify(words), 0x00011234ABCDULL);
}

TEST("ahci: the model string un-swaps each word on the way out") {
    std::array<uint16_t, 256> words = {};
    words[27]                       = 0x5145;
    words[28]                       = 0x5551;
    std::array<char, 41> model      = {};
    cinux::driver::ModelFromIdentify(words, model);
    ASSERT_EQ(model[0], 'Q');
    ASSERT_EQ(model[1], 'E');
    ASSERT_EQ(model[2], 'U');
    ASSERT_EQ(model[3], 'Q');
    ASSERT_EQ(model[4], '\0');
}

namespace {

std::array<uint16_t, 256> plain_ata_drive() {
    std::array<uint16_t, 256> words = {};
    words[0]                        = 0x0040;
    for (unsigned long index = 0; index < 20; ++index) {
        words[27 + index] = 0x2020;
    }
    words[27]  = 0x5145;
    words[28]  = 0x5551;
    words[83]  = 0x4400;
    words[100] = 2048;
    return words;
}

}  // namespace

TEST("ahci: a plain LBA48 drive with a printable model admits") {
    const std::array<uint16_t, 256> kWords = plain_ata_drive();
    ASSERT_TRUE(cinux::driver::IdentifyAdmits(kWords));
}

TEST("ahci: CFA and incomplete identify words refuse admission") {
    std::array<uint16_t, 256> words = plain_ata_drive();
    words[0]                        = 0x8040;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
    words    = plain_ata_drive();
    words[0] = 0x0044;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
}

TEST("ahci: a blank or unprintable model refuses admission") {
    std::array<uint16_t, 256> words = plain_ata_drive();
    words[27]                       = 0x2020;
    words[28]                       = 0x2020;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
    words     = plain_ata_drive();
    words[27] = 0x00E9;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
}

TEST("ahci: a drive without valid LBA48 words refuses admission") {
    std::array<uint16_t, 256> words = plain_ata_drive();
    words[83]                       = 0x4000;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
    words     = plain_ata_drive();
    words[83] = 0x8400;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
}

TEST("ahci: a marked checksum that does not sum out refuses admission") {
    std::array<uint16_t, 256> words = plain_ata_drive();
    words[255]                      = 0x00A5;
    ASSERT_FALSE(cinux::driver::IdentifyAdmits(words));
}

int main() {
    return cinux::test::RunAll();
}
