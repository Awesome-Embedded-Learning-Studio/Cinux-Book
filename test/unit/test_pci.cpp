#include <stdint.h>

#include <array>

#include "../framework/framework.hpp"
#include "kernel/driver/pci/pci_layout.hpp"
#include "test_assert.hpp"

TEST("pci: config address word packs location, register and enable bit") {
    const uint32_t kWord = cinux::driver::MakeConfigAddress(2, 19, 1, 0x3C);
    ASSERT_EQ(kWord, 0x8002993CU);
}

TEST("pci: misaligned register offsets fall back to their dword") {
    const uint32_t kWord = cinux::driver::MakeConfigAddress(0, 0, 0, 0x3F);
    ASSERT_EQ(kWord, 0x8000003CU);
}

TEST("pci: an idle bar decodes as unused") {
    const cinux::driver::PciBar kBar = cinux::driver::DecodeBar(0);
    ASSERT_EQ(static_cast<int>(kBar.kind), static_cast<int>(cinux::driver::PciBarKind::kUnused));
    ASSERT_EQ(kBar.base, 0U);
}

TEST("pci: an io bar keeps its base and drops the flag bits") {
    const cinux::driver::PciBar kBar = cinux::driver::DecodeBar(0x0000C001);
    ASSERT_EQ(static_cast<int>(kBar.kind), static_cast<int>(cinux::driver::PciBarKind::kIo));
    ASSERT_EQ(kBar.base, 0xC000U);
    ASSERT_FALSE(kBar.prefetchable);
}

TEST("pci: a 32-bit memory bar decodes its base") {
    const cinux::driver::PciBar kBar = cinux::driver::DecodeBar(0xFEBF1000);
    ASSERT_EQ(static_cast<int>(kBar.kind), static_cast<int>(cinux::driver::PciBarKind::kMemory32));
    ASSERT_EQ(kBar.base, 0xFEBF1000U);
}

TEST("pci: prefetchable and 64-bit memory bars decode their flags") {
    const cinux::driver::PciBar kPrefetch = cinux::driver::DecodeBar(0xFD000008);
    ASSERT_TRUE(kPrefetch.prefetchable);
    ASSERT_EQ(static_cast<int>(kPrefetch.kind),
              static_cast<int>(cinux::driver::PciBarKind::kMemory32));

    const cinux::driver::PciBar kWide = cinux::driver::DecodeBar(0xFEBC0004);
    ASSERT_EQ(static_cast<int>(kWide.kind), static_cast<int>(cinux::driver::PciBarKind::kMemory64));
    ASSERT_FALSE(kWide.prefetchable);
}

TEST("pci: a 64-bit bar takes its high half from the next register") {
    const std::array<uint32_t, 6> kBars = {0xFEBC0004, 0x00000001, 0, 0, 0, 0};
    ASSERT_EQ(cinux::driver::BarBase(kBars, 0), 0x1FEBC0000ULL);
}

TEST("pci: a 32-bit bar ignores the next register as a high half") {
    const std::array<uint32_t, 6> kBars = {0xFEBF1000, 0xDEADBEEF, 0, 0, 0, 0};
    ASSERT_EQ(cinux::driver::BarBase(kBars, 0), 0xFEBF1000ULL);
}

TEST("pci: storage classes carry their spec numbers") {
    ASSERT_EQ(static_cast<unsigned>(cinux::driver::PciClassCode::kMassStorage), 0x01U);
    ASSERT_EQ(static_cast<unsigned>(cinux::driver::PciClassCode::kBridge), 0x06U);
    ASSERT_EQ(cinux::driver::kPciSubclassIde, 0x01U);
    ASSERT_EQ(cinux::driver::kPciSubclassAhci, 0x06U);
    ASSERT_EQ(cinux::driver::kPciSubclassNvme, 0x08U);
}

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

TEST("pci: the capability walk finds the ids the list names") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x40U;
    words[0x40 / 4U]                       = 0x00005011U;
    words[0x50 / 4U]                       = 0x00000005U;

    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 2, .func = 1};
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x11U), 0x40U);
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x05U), 0x50U);
}

TEST("pci: the walk answers zero without a list or without the id") {
    uint32_t words[64]                     = {};
    words[kCapabilityPointerRegister / 4U] = 0x40U;
    words[0x40 / 4U]                       = 0x00000011U;

    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x11U), 0U);

    words[kStatusRegister / 4U] = kStatusHasCaps;
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x05U), 0U);
}

TEST("pci: a circling list stops at the walk limit") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x40U;
    words[0x40 / 4U]                       = 0x00400011U;

    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x05U), 0U);
}

TEST("pci: a pointer's reserved low bits read as cleared") {
    uint32_t words[64]                     = {};
    words[kStatusRegister / 4U]            = kStatusHasCaps;
    words[kCapabilityPointerRegister / 4U] = 0x41U;
    words[0x40 / 4U]                       = 0x00000005U;

    const cinux::driver::PciSpot kSpot{.bus = 0, .slot = 0, .func = 0};
    ASSERT_EQ(cinux::driver::FindCapability(&read_mock_words, words, kSpot, 0x05U), 0x40U);
}

int main() {
    return cinux::test::RunAll();
}
