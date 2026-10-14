#include <cstddef>
#include <cstdint>

#include "../framework/framework.hpp"
#include "cinux/byte_order.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "test_assert.hpp"

namespace {

constexpr std::size_t kEhdrBytes    = 64;
constexpr std::size_t kPhdrBytes    = 56;
constexpr std::size_t kPayloadBytes = 56;
constexpr std::size_t kImageBytes   = kEhdrBytes + kPhdrBytes + kPayloadBytes;

struct TestImage {
    std::uint8_t bytes[kImageBytes] = {};
};

constexpr void put_word(TestImage& image, std::size_t where, std::uint16_t value) {
    cinux::base::WriteWord16(image.bytes, static_cast<std::uint32_t>(where), value);
}

constexpr void put_dword(TestImage& image, std::size_t where, std::uint32_t value) {
    cinux::base::WriteWord32(image.bytes, static_cast<std::uint32_t>(where), value);
}

constexpr void put_qword(TestImage& image, std::size_t where, std::uint64_t value) {
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

void check_good_segment(const cinux::loader::ElfImage& image) {
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(cinux::loader::ReadSegment(image, 0, segment));
    ASSERT_TRUE(segment.virtual_address == 0x401000);
    ASSERT_TRUE(segment.file_offset == kEhdrBytes + kPhdrBytes);
    ASSERT_TRUE(segment.file_bytes == kPayloadBytes);
    ASSERT_TRUE(segment.memory_bytes == 96);
}

TEST("elf: a well-formed static image parses to entry and segments") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(kParsed.value().entry == 0x401000);
    ASSERT_TRUE(kParsed.value().segment_count == 1);
    ASSERT_TRUE(kParsed.value().segment_size == 56);
    check_good_segment(kParsed.value());
}

void refuse_mangled_word(TestImage broken, std::size_t where, std::uint8_t value) {
    broken.bytes[where] = value;
    ASSERT_TRUE(!cinux::loader::ParseElf64(broken.bytes, kImageBytes).ok());
}

TEST("elf: refusals name their family") {
    ASSERT_TRUE(!cinux::loader::ParseElf64(nullptr, 64).ok());
    ASSERT_TRUE(!cinux::loader::ParseElf64(kGoodImage.bytes, 32).ok());
    refuse_mangled_word(kGoodImage, 0, 'M');
    refuse_mangled_word(kGoodImage, 4, 1);
    refuse_mangled_word(kGoodImage, 16, 3);
    refuse_mangled_word(kGoodImage, 18, 0x28);
    refuse_mangled_word(kGoodImage, 56, 0);
    refuse_mangled_word(kGoodImage, 54, 32);
}

TEST("elf: reads past the table refuse politely") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes);
    ASSERT_TRUE(kParsed.ok());
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(!cinux::loader::ReadSegment(kParsed.value(), 1, segment));
    ASSERT_TRUE(!cinux::loader::ReadSegment(kParsed.value(), 999, segment));
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
