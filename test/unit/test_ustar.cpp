#include <cstring>
#include <string_view>
#include <vector>

#include "../framework/framework.hpp"
#include "cinux/math.hpp"
#include "cinux/parse.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/ustar.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned long kBlock = 512;

void append_bytes(std::vector<unsigned char>& out, unsigned long bytes) {
    out.insert(out.end(), bytes, 0);
}

void append_header(std::vector<unsigned char>& out, const char* name, unsigned long size,
                   char flag) {
    const unsigned long kHead = out.size();
    append_bytes(out, kBlock);

    const unsigned long kNameLen = std::strlen(name);
    for (unsigned long index = 0; index < kNameLen; ++index) {
        out[kHead + index] = static_cast<unsigned char>(name[index]);
    }

    char          octal[13] = {};
    unsigned long rest      = size;
    for (int digit = 10; digit >= 0; --digit) {
        octal[digit] = static_cast<char>('0' + (rest % 8));
        rest /= 8;
    }
    for (int index = 0; index < 12; ++index) {
        out[kHead + 124 + index] = static_cast<unsigned char>(octal[index]);
    }

    out[kHead + 156] = static_cast<unsigned char>(flag);
    for (int index = 0; index < 6; ++index) {
        out[kHead + 257 + index] = static_cast<unsigned char>("ustar"[index]);
    }
}

void append_data(std::vector<unsigned char>& out, const char* bytes, unsigned long count) {
    const unsigned long kPadded = cinux::base::math::Ceil(count, kBlock) * kBlock;
    append_bytes(out, kPadded);
    for (unsigned long index = 0; index < count; ++index) {
        out[out.size() - kPadded + index] = static_cast<unsigned char>(bytes[index]);
    }
}

std::vector<unsigned char> finished_image() {
    std::vector<unsigned char> image;
    append_bytes(image, kBlock);
    append_bytes(image, kBlock);
    return image;
}

}  // namespace

TEST("ustar: octal fields parse with zeros, spaces and NULs") {
    using cinux::base::ParseOctal;
    const auto kZero  = ParseOctal("00000000000");
    const auto kValue = ParseOctal("00000001240");
    const auto kSpace = ParseOctal("00000000012 ");
    const auto kBad   = ParseOctal("00000001290");
    const auto kHex   = ParseOctal("0x00000000");
    ASSERT_TRUE(kZero.ok());
    ASSERT_EQ(kZero.value(), 0UL);
    ASSERT_EQ(kValue.value(), 672UL);
    ASSERT_EQ(kSpace.value(), 10UL);
    ASSERT_FALSE(kBad.ok());
    ASSERT_FALSE(kHex.ok());
}

TEST("ustar: file entry hands out name, size and type") {
    std::vector<unsigned char> image;
    append_header(image, "hello.txt", 5, '0');
    append_data(image, "hello", 5);

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    const auto             kFirst = reader.next(entry);
    ASSERT_TRUE(kFirst.ok());
    ASSERT_TRUE(kFirst.value());
    ASSERT_EQ(entry.name, std::string_view("hello.txt"));
    ASSERT_EQ(entry.size, 5UL);
    ASSERT_TRUE(entry.type == cinux::fs::InodeType::kFile);
}

TEST("ustar: entry data views the archive bytes in place") {
    std::vector<unsigned char> image;
    append_header(image, "hello.txt", 5, '0');
    append_data(image, "hello", 5);

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    ASSERT_TRUE(reader.next(entry).value());
    ASSERT_EQ(entry.data.size(), 5UL);
    ASSERT_TRUE(std::memcmp(entry.data.data(), "hello", 5) == 0);
}

TEST("ustar: zero blocks end the walk, and it stays ended") {
    std::vector<unsigned char> image;
    append_header(image, "hello.txt", 5, '0');
    append_data(image, "hello", 5);
    append_bytes(image, kBlock);
    append_bytes(image, kBlock);

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    ASSERT_TRUE(reader.next(entry).value());
    const auto kAtEnd = reader.next(entry);
    ASSERT_TRUE(kAtEnd.ok());
    ASSERT_FALSE(kAtEnd.value());
    const auto kStill = reader.next(entry);
    ASSERT_TRUE(kStill.ok());
    ASSERT_FALSE(kStill.value());
}

TEST("ustar: padding lands the cursor on the next header") {
    std::vector<unsigned char> image;
    append_header(image, "a.txt", 3, '0');
    append_data(image, "aaa", 3);
    append_header(image, "dir", 0, '5');
    const std::vector<unsigned char> kEnd = finished_image();
    image.insert(image.end(), kEnd.begin(), kEnd.end());

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    const auto             kFile = reader.next(entry);
    ASSERT_TRUE(kFile.ok());
    ASSERT_TRUE(kFile.value());
    ASSERT_EQ(entry.name, std::string_view("a.txt"));

    const auto kDir = reader.next(entry);
    ASSERT_TRUE(kDir.ok());
    ASSERT_TRUE(kDir.value());
    ASSERT_EQ(entry.name, std::string_view("dir"));
}

TEST("ustar: a directory entry reads as size zero") {
    std::vector<unsigned char> image;
    append_header(image, "dir", 0, '5');
    const std::vector<unsigned char> kEnd = finished_image();
    image.insert(image.end(), kEnd.begin(), kEnd.end());

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    const auto             kDir = reader.next(entry);
    ASSERT_TRUE(kDir.ok());
    ASSERT_TRUE(kDir.value());
    ASSERT_EQ(entry.name, std::string_view("dir"));
    ASSERT_TRUE(entry.type == cinux::fs::InodeType::kDirectory);
    ASSERT_EQ(entry.size, 0UL);
}

TEST("ustar: bad magic is refused, not guessed") {
    std::vector<unsigned char> image;
    append_header(image, "a.txt", 0, '0');
    image[257] = 'x';
    append_bytes(image, kBlock);

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    const auto             kVerdict = reader.next(entry);
    ASSERT_FALSE(kVerdict.ok());
    ASSERT_EQ(kVerdict.error(), cinux::base::KernelError::kInvalidArgument);
}

TEST("ustar: truncated archive is an error, not a short entry") {
    std::vector<unsigned char> image;
    append_header(image, "a.txt", 600, '0');
    append_bytes(image, kBlock);
    image.resize(image.size() - 1);

    cinux::fs::UstarReader reader(image.data(), image.size());
    cinux::fs::UstarEntry  entry;
    ASSERT_FALSE(reader.next(entry).ok());
}

int main() {
    return cinux::test::RunAll();
}
