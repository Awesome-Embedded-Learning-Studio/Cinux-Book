#include <stdint.h>

#include <array>

#include "../framework/framework.hpp"
#include "kernel/driver/msi/msi_layout.hpp"
#include "kernel/driver/msi/msix.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "test_assert.hpp"

namespace {

/// @brief Register offset: command and status share this dword.
constexpr uint8_t kStatusRegister = 0x04;

/// @brief Register offset: the capability list's first pointer lives here.
constexpr uint8_t kCapabilityPointerRegister = 0x34;

/// @brief Status word bit that promises a capability list.
constexpr uint32_t kStatusHasCaps = 0x00100000U;

uint32_t read_mock_words(const void* context, cinux::driver::PciSpot /*spot*/, uint8_t offset) {
    const auto* const kWords = static_cast<const uint32_t*>(context);
    return kWords[offset / 4U];
}

}  // namespace

TEST("msi-x: the capability decodes its table shape") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x70U;
    words[0x70 / 4U]                       = 0x80070011U;
    words[0x74 / 4U]                       = 0x00008001U;
    words[0x78 / 4U]                       = 0x00010002U;

    cinux::driver::msi::MsixFacts facts{};
    const cinux::driver::PciSpot  kSpot{.bus = 0, .slot = 31, .func = 7};
    ASSERT_TRUE(cinux::driver::msi::DecodeMsix(&read_mock_words, words, kSpot, facts));
    ASSERT_EQ(facts.table_size, 8U);
    ASSERT_TRUE(facts.enabled);
    ASSERT_FALSE(facts.masked);
    ASSERT_EQ(facts.table_bir, 1U);
    ASSERT_EQ(facts.table_offset, 0x1000U);
    ASSERT_EQ(facts.pending_bir, 2U);
    ASSERT_EQ(facts.pending_offset, 0x2000U);
}

TEST("msi-x: the function mask decodes alongside a single entry") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x70U;
    words[0x70 / 4U]                       = 0x40000011U;

    cinux::driver::msi::MsixFacts facts{};
    const cinux::driver::PciSpot  kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_TRUE(cinux::driver::msi::DecodeMsix(&read_mock_words, words, kSpot, facts));
    ASSERT_EQ(facts.table_size, 1U);
    ASSERT_FALSE(facts.enabled);
    ASSERT_TRUE(facts.masked);
}

TEST("msi: a narrow capability decodes address and data words") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x40U;
    words[0x40 / 4U]                       = 0x01510005U;
    words[0x44 / 4U]                       = 0xFEE00000U;
    words[0x48 / 4U]                       = 0x00000040U;

    cinux::driver::msi::MsiFacts facts{};
    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_TRUE(cinux::driver::msi::DecodeMsi(&read_mock_words, words, kSpot, facts));
    ASSERT_TRUE(facts.enabled);
    ASSERT_TRUE(facts.maskable);
    ASSERT_FALSE(facts.wide_address);
    ASSERT_EQ(facts.vector_cap, 5U);
    ASSERT_EQ(facts.address, 0xFEE00000U);
    ASSERT_EQ(facts.address_high, 0U);
    ASSERT_EQ(facts.data, 0x40U);
}

TEST("msi: a wide capability takes its data after the high half") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x40U;
    words[0x40 / 4U]                       = 0x01D10005U;
    words[0x44 / 4U]                       = 0xFEE00000U;
    words[0x48 / 4U]                       = 0x00000001U;
    words[0x4C / 4U]                       = 0x00000041U;

    cinux::driver::msi::MsiFacts facts{};
    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_TRUE(cinux::driver::msi::DecodeMsi(&read_mock_words, words, kSpot, facts));
    ASSERT_TRUE(facts.wide_address);
    ASSERT_EQ(facts.address, 0xFEE00000U);
    ASSERT_EQ(facts.address_high, 1U);
    ASSERT_EQ(facts.data, 0x41U);
}

TEST("msi: decoding answers false without the capability") {
    uint32_t words[64] = {};

    const cinux::driver::PciSpot  kSpot{.bus = 0, .slot = 0, .func = 0};
    cinux::driver::msi::MsiFacts  narrow{};
    cinux::driver::msi::MsixFacts tables{};
    ASSERT_FALSE(cinux::driver::msi::DecodeMsi(&read_mock_words, words, kSpot, narrow));
    ASSERT_FALSE(cinux::driver::msi::DecodeMsix(&read_mock_words, words, kSpot, tables));
}

TEST("msi-x: table windows land inside their base registers") {
    const cinux::driver::msi::MsixFacts kFacts{
        .table_size     = 8,
        .enabled        = true,
        .masked         = false,
        .table_bir      = 1,
        .table_offset   = 0x1000,
        .pending_bir    = 0,
        .pending_offset = 0x2000,
    };
    const std::array<uint32_t, 6> kBars = {0xFD000000U, 0xFEBC0004U, 0x00000001U, 0, 0, 0};

    const cinux::driver::msi::MsixWindows kWindows =
        cinux::driver::msi::ResolveMsixWindows(kFacts, kBars);
    ASSERT_EQ(kWindows.table, 0x1FEBC1000ULL);
    ASSERT_EQ(kWindows.pending, 0xFD002000ULL);
}

TEST("msi: the xapic message address lands on the local controller") {
    ASSERT_EQ(cinux::driver::msi::XapicMessageAddress(0), 0xFEE00000ULL);
    ASSERT_EQ(cinux::driver::msi::XapicMessageAddress(2), 0xFEE02000ULL);
}

TEST("msi-x: arming an entry lands the pair unmasked") {
    cinux::driver::msi::MsixEntry entry{};
    ASSERT_TRUE(cinux::driver::msi::ArmMsixEntry(entry, 0xFEE02000ULL, 0x40U));
    ASSERT_EQ(entry.address, 0xFEE02000ULL);
    ASSERT_EQ(entry.data, 0x40U);
    ASSERT_EQ(entry.control, 0U);
}

TEST("msi-x: an entry that dropped its pair answers unlatched") {
    cinux::driver::msi::MsixEntry entry{};
    ASSERT_FALSE(cinux::driver::msi::EntryLatched(entry, 0xFEE02000ULL, 0x40U));
    entry.address = 0xFEE02000ULL;
    ASSERT_FALSE(cinux::driver::msi::EntryLatched(entry, 0xFEE02000ULL, 0x40U));
    entry.data = 0x40U;
    ASSERT_TRUE(cinux::driver::msi::EntryLatched(entry, 0xFEE02000ULL, 0x40U));
}

int main() {
    return cinux::test::RunAll();
}
