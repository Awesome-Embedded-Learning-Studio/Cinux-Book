#include <cstddef>
#include <cstdint>

#include "cinux/byte_order.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr std::size_t kEhdrBytes    = 64;
constexpr std::size_t kPhdrBytes    = 56;
constexpr std::size_t kPayloadBytes = 56;
constexpr std::size_t kImageBytes   = kEhdrBytes + kPhdrBytes + kPayloadBytes;

struct TestImage {
    std::uint8_t bytes[kImageBytes] = {};
};

template <typename Image>
constexpr void put_word(Image& image, std::size_t where, std::uint16_t value) {
    cinux::base::WriteWord16(image.bytes, static_cast<std::uint32_t>(where), value);
}

template <typename Image>
constexpr void put_dword(Image& image, std::size_t where, std::uint32_t value) {
    cinux::base::WriteWord32(image.bytes, static_cast<std::uint32_t>(where), value);
}

template <typename Image>
constexpr void put_qword(Image& image, std::size_t where, std::uint64_t value) {
    put_dword(image, where, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
    put_dword(image, where + 4, static_cast<std::uint32_t>(value >> 32U));
}

constexpr TestImage build_image() {
    TestImage image{};
    image.bytes[0] = 0x7F;
    image.bytes[1] = 'E';
    image.bytes[2] = 'L';
    image.bytes[3] = 'F';
    image.bytes[4] = 2;
    image.bytes[5] = 1;
    image.bytes[6] = 1;
    put_word(image, 16, 2);
    put_word(image, 18, 0x3E);
    put_dword(image, 20, 1);
    put_qword(image, 24, 0x401000);
    put_qword(image, 32, 64);
    put_qword(image, 40, 0);
    put_word(image, 52, 64);
    put_word(image, 54, 56);
    put_word(image, 56, 1);
    put_dword(image, 64, 1);
    put_dword(image, 68, 5);
    put_qword(image, 72, kEhdrBytes + kPhdrBytes);
    put_qword(image, 80, 0x401000);
    put_qword(image, 88, 0x401000);
    put_qword(image, 96, kPayloadBytes);
    put_qword(image, 104, 96);
    put_qword(image, 112, 0x1000);
    for (std::size_t index = 0; index < kPayloadBytes; ++index) {
        image.bytes[kEhdrBytes + kPhdrBytes + index] = static_cast<std::uint8_t>(0x10 + index);
    }
    return image;
}

constexpr TestImage kGoodImage = build_image();

constexpr std::size_t kMidOffsetBytes = 56;
constexpr std::size_t kMidImageBytes  = kEhdrBytes + (2 * kPhdrBytes) + kMidOffsetBytes;

struct MidImage {
    std::uint8_t bytes[kMidImageBytes] = {};
};

constexpr MidImage build_mid_image() {
    MidImage image{};
    image.bytes[0] = 0x7F;
    image.bytes[1] = 'E';
    image.bytes[2] = 'L';
    image.bytes[3] = 'F';
    image.bytes[4] = 2;
    image.bytes[5] = 1;
    image.bytes[6] = 1;
    put_word(image, 16, 2);
    put_word(image, 18, 0x3E);
    put_dword(image, 20, 1);
    put_qword(image, 24, 0x401BF0);
    put_qword(image, 32, 64);
    put_qword(image, 40, 0);
    put_word(image, 52, 64);
    put_word(image, 54, 56);
    put_word(image, 56, 1);
    const std::size_t kFirst = kEhdrBytes;
    put_dword(image, kFirst, 1);
    put_dword(image, kFirst + 4, 6);
    put_qword(image, kFirst + 8, kEhdrBytes + kPhdrBytes);
    put_qword(image, kFirst + 16, 0x401BF0);
    put_qword(image, kFirst + 24, 0x401BF0);
    put_qword(image, kFirst + 32, 24);
    put_qword(image, kFirst + 40, 0x120);
    put_qword(image, kFirst + 48, 0x1000);
    for (std::size_t index = 0; index < kMidOffsetBytes; ++index) {
        image.bytes[kEhdrBytes + kPhdrBytes + index] = static_cast<std::uint8_t>(0x30 + index);
    }
    return image;
}

constexpr MidImage kMidSegImage = build_mid_image();

constexpr TestImage build_shared_image() {
    TestImage image{};
    image.bytes[0] = 0x7F;
    image.bytes[1] = 'E';
    image.bytes[2] = 'L';
    image.bytes[3] = 'F';
    image.bytes[4] = 2;
    image.bytes[5] = 1;
    image.bytes[6] = 1;
    put_word(image, 16, 3);
    put_word(image, 18, 0x3E);
    put_dword(image, 20, 1);
    put_qword(image, 24, 0x2000);
    put_qword(image, 32, 64);
    put_qword(image, 40, 0);
    put_word(image, 52, 64);
    put_word(image, 54, 56);
    put_word(image, 56, 1);
    put_dword(image, 64, 1);
    put_dword(image, 68, 5);
    put_qword(image, 72, 0);
    put_qword(image, 80, 0);
    put_qword(image, 88, 0);
    put_qword(image, 96, kImageBytes);
    put_qword(image, 104, kImageBytes);
    put_qword(image, 112, 0x1000);
    for (std::size_t index = 0; index < kPayloadBytes; ++index) {
        image.bytes[kEhdrBytes + kPhdrBytes + index] = static_cast<std::uint8_t>(0x30 + index);
    }
    return image;
}

constexpr TestImage kSharedImage = build_shared_image();

void check_payload_word() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401000), 0x13121110U);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401008), 0x1B1A1918U);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint8_t>(0x401037), 0x47U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

void check_bss_zero() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401038), 0ULL);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401040), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401058), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint8_t>(0x40105F), 0U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

TEST("loader: segments land page-aligned and bss arrives zero") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore            vmas;
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{.space = space, .vmas = vmas, .file = {}};
    ASSERT_TRUE(cinux::loader::LoadElfIntoSpace(kGoodImage.bytes, kImageBytes, kWorld, facts));
    ASSERT_EQ(facts.program_entry, 0x401000ULL);
    space.activate();
    check_payload_word();
    check_bss_zero();
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

void check_mid_head_data() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401BF0), 0x33323130U);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401C00), 0x43424140U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

void check_mid_around_zero() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401000), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401BE8), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401C08), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401D08), 0ULL);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

TEST("loader: a segment mid-page keeps its head page's file bytes") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore            vmas;
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{.space = space, .vmas = vmas, .file = {}};
    ASSERT_TRUE(cinux::loader::LoadElfIntoSpace(kMidSegImage.bytes, kMidImageBytes, kWorld, facts));
    ASSERT_EQ(facts.program_entry, 0x401BF0ULL);
    space.activate();
    check_mid_head_data();
    check_mid_around_zero();
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("loader: an interpreter-seat image lands at its base") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore            vmas;
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{
        .space = space,
        .vmas  = vmas,
        .file  = {},
        .role  = cinux::loader::ImageRole::kInterpreter,
        .base  = cinux::mm::kUserInterpreterBase,
    };
    ASSERT_TRUE(cinux::loader::LoadElfIntoSpace(kSharedImage.bytes, kImageBytes, kWorld, facts));
    ASSERT_EQ(facts.program_entry, cinux::mm::kUserInterpreterBase + 0x2000ULL);
    ASSERT_EQ(facts.program_headers, cinux::mm::kUserInterpreterBase + kEhdrBytes);
    space.activate();
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(cinux::mm::kUserInterpreterBase + kEhdrBytes +
                                                 kPhdrBytes),
              0x33323130U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    space.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

}  // namespace
