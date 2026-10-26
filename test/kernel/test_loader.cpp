#include <cstddef>
#include <cstdint>

#include "cinux/byte_order.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/fs/ramfs/ramfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/demand.hpp"
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
    put_qword(image, 72, 0);
    put_qword(image, 80, 0x401000);
    put_qword(image, 88, 0x401000);
    put_qword(image, 96, kImageBytes);
    put_qword(image, 104, kImageBytes + 40);
    put_qword(image, 112, 0x1000);
    for (std::size_t index = 0; index < kPayloadBytes; ++index) {
        image.bytes[kEhdrBytes + kPhdrBytes + index] = static_cast<std::uint8_t>(0x10 + index);
    }
    return image;
}

constexpr TestImage kGoodImage = build_image();

constexpr std::size_t kMidSegOffset  = 0xF0;
constexpr std::size_t kMidSegBytes   = 24;
constexpr std::size_t kMidImageBytes = kMidSegOffset + kMidSegBytes;

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
    put_qword(image, 24, 0x4010F0);
    put_qword(image, 32, 64);
    put_qword(image, 40, 0);
    put_word(image, 52, 64);
    put_word(image, 54, 56);
    put_word(image, 56, 1);
    const std::size_t kFirst = kEhdrBytes;
    put_dword(image, kFirst, 1);
    put_dword(image, kFirst + 4, 6);
    put_qword(image, kFirst + 8, kMidSegOffset);
    put_qword(image, kFirst + 16, 0x4010F0);
    put_qword(image, kFirst + 24, 0x4010F0);
    put_qword(image, kFirst + 32, kMidSegBytes);
    put_qword(image, kFirst + 40, 0x120);
    put_qword(image, kFirst + 48, 0x1000);
    for (std::size_t index = 0; index < kMidSegBytes; ++index) {
        image.bytes[kMidSegOffset + index] = static_cast<std::uint8_t>(0x40 + index);
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
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401078), 0x13121110U);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401080), 0x1B1A1918U);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint8_t>(0x4010AF), 0x47U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

void check_bss_zero() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x4010B0), 0ULL);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x4010B8), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x4010D0), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint8_t>(0x4010D7), 0U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

TEST("loader: segments land page-aligned and bss arrives zero") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::fs::RamFs                  volume;
    cinux::mm::VmaStore               vmas;
    static constexpr cinux::fs::FsOps kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const auto                        kFile = volume.make_file("image");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(volume.write(kFile.value(), 0, kGoodImage.bytes, kImageBytes).ok());
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{
        .space      = space,
        .vmas       = vmas,
        .file       = kFile.value(),
        .file_bytes = kImageBytes,
        .file_ops   = &kOps,
        .file_self  = &volume,
    };
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
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x4010F0), 0x43424140U);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401100), 0x53525150U);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

void check_mid_head_page_keeps_file() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint32_t>(0x401000), 0x464C457FU);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

void check_mid_around_zero() {
    // NOLINTBEGIN(clang-analyzer-optin.core.FixedAddressDereference)
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401108), 0ULL);
    ASSERT_EQ(*cinux::base::PtrAt<std::uint64_t>(0x401210), 0ULL);
    // NOLINTEND(clang-analyzer-optin.core.FixedAddressDereference)
}

TEST("loader: a segment mid-page keeps its head page's file bytes") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::fs::RamFs                  volume;
    cinux::mm::VmaStore               vmas;
    static constexpr cinux::fs::FsOps kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const auto                        kFile = volume.make_file("image");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(volume.write(kFile.value(), 0, kMidSegImage.bytes, kMidImageBytes).ok());
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{
        .space      = space,
        .vmas       = vmas,
        .file       = kFile.value(),
        .file_bytes = kMidImageBytes,
        .file_ops   = &kOps,
        .file_self  = &volume,
    };
    ASSERT_TRUE(cinux::loader::LoadElfIntoSpace(kMidSegImage.bytes, kMidImageBytes, kWorld, facts));
    ASSERT_EQ(facts.program_entry, 0x4010F0ULL);
    space.activate();
    check_mid_head_data();
    check_mid_head_page_keeps_file();
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
    cinux::fs::RamFs                  volume;
    cinux::mm::VmaStore               vmas;
    static constexpr cinux::fs::FsOps kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const auto                        kFile = volume.make_file("image");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(volume.write(kFile.value(), 0, kSharedImage.bytes, kImageBytes).ok());
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{
        .space      = space,
        .vmas       = vmas,
        .file       = kFile.value(),
        .file_bytes = kImageBytes,
        .file_ops   = &kOps,
        .file_self  = &volume,
        .role       = cinux::loader::ImageRole::kInterpreter,
        .base       = cinux::mm::kUserInterpreterBase,
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

struct LargeImage {
    std::uint8_t bytes[0x5000]{};
};

constexpr LargeImage build_large_image() {
    LargeImage image{};
    for (unsigned long index = 0; index < kImageBytes; ++index) {
        image.bytes[index] = kSharedImage.bytes[index];
    }
    put_word(image, 16, 2);
    put_qword(image, 80, 0x401000);
    put_qword(image, 96, 0x1080);
    put_qword(image, 104, 0x4000);
    for (unsigned long index = kImageBytes; index < sizeof(image.bytes); ++index) {
        image.bytes[index] = 0xA5;
    }
    return image;
}

constexpr auto kLargeImage = build_large_image();

void check_lazy_tail(const cinux::mm::AddressSpace& space) {
    ASSERT_EQ(space.translate(0x401000), 0UL);
    ASSERT_EQ(space.translate(0x403000), 0UL);
    ASSERT_TRUE(space.translate(0x402000) != 0);
    const auto kTail = space.translate(0x402000);
    ASSERT_EQ(*cinux::base::PtrAt<unsigned char>(cinux::mm::DirectMapVirt(kTail) + 0x7F), 0xA5U);
    ASSERT_EQ(*cinux::base::PtrAt<unsigned char>(cinux::mm::DirectMapVirt(kTail) + 0x80), 0U);
}

void check_demanded_contents(const cinux::mm::AddressSpace& space,
                             const cinux::mm::VmaStore&     copy) {
    ASSERT_TRUE(
        cinux::mm::demand::FillPage(space, copy, 0x401000, cinux::arch::isr::kFaultFromUser));
    ASSERT_EQ(
        *cinux::base::PtrAt<std::uint32_t>(cinux::mm::DirectMapVirt(space.translate(0x401000))),
        0x464C457FU);
    ASSERT_TRUE(
        cinux::mm::demand::FillPage(space, copy, 0x403000, cinux::arch::isr::kFaultFromUser));
    ASSERT_EQ(*cinux::base::PtrAt<unsigned long long>(
                  cinux::mm::DirectMapVirt(space.translate(0x403000))),
              0ULL);
}

TEST("loader: lazy file pages and BSS ignore trailing file garbage across fork and protect") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::fs::RamFs      volume;
    cinux::mm::VmaStore   vmas;
    static constexpr auto kOps  = cinux::fs::EraseFs<cinux::fs::RamFs>();
    const auto            kFile = volume.make_file("image");
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(volume.write(kFile.value(), 0, kLargeImage.bytes, sizeof(kLargeImage.bytes)).ok());
    const cinux::loader::LoadWorld kWorld{
        .space      = space,
        .vmas       = vmas,
        .file       = kFile.value(),
        .file_bytes = sizeof(kLargeImage.bytes),
        .file_ops   = &kOps,
        .file_self  = &volume,
    };
    cinux::loader::EntryFacts facts{};
    ASSERT_TRUE(cinux::loader::LoadElfIntoSpace(kLargeImage.bytes, 120, kWorld, facts));
    ASSERT_TRUE(volume.unlink("image").ok());
    check_lazy_tail(space);
    ASSERT_TRUE(vmas.protect(0x403000, 0x404000, {.readable = true}).ok());
    const auto* const kCopy = vmas.clone();
    ASSERT_TRUE(kCopy != nullptr);
    check_demanded_contents(space, *kCopy);
    delete kCopy;
}

TEST("loader: malformed segment bounds and incongruent offsets refuse without mapping") {
    cinux::mm::AddressSpace space;
    ASSERT_TRUE(space.init());
    cinux::mm::VmaStore            vmas;
    cinux::loader::EntryFacts      facts{};
    const cinux::loader::LoadWorld kWorld{.space = space, .vmas = vmas, .file_bytes = kImageBytes};
    auto                           image = kGoodImage;
    put_qword(image, 72, 1);
    ASSERT_FALSE(cinux::loader::LoadElfIntoSpace(image.bytes, kImageBytes, kWorld, facts));
    put_qword(image, 72, 0);
    put_qword(image, 96, kImageBytes + 1);
    ASSERT_FALSE(cinux::loader::LoadElfIntoSpace(image.bytes, kImageBytes, kWorld, facts));
    ASSERT_TRUE(vmas.empty());
}

}  // namespace
