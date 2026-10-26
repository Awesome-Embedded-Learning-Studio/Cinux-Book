#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "../framework/framework.hpp"
#include "cinux/byte_order.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/loader/exec_arguments.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/proc/proc_config.hpp"
#include "test_assert.hpp"

namespace {

static_assert(cinux::loader::kTypeExecutable == ET_EXEC);
static_assert(cinux::loader::kTypeShared == ET_DYN);
static_assert(cinux::loader::kProgramLoad == PT_LOAD);
static_assert(cinux::loader::kProgramInterp == PT_INTERP);
static_assert(cinux::loader::kProgramGnuStack == PT_GNU_STACK);
static_assert(cinux::loader::kAuxNull == AT_NULL);
static_assert(cinux::loader::kAuxPhdr == AT_PHDR);
static_assert(cinux::loader::kAuxPhent == AT_PHENT);
static_assert(cinux::loader::kAuxPhnum == AT_PHNUM);
static_assert(cinux::loader::kAuxPageSize == AT_PAGESZ);
static_assert(cinux::loader::kAuxBase == AT_BASE);
static_assert(cinux::loader::kAuxEntry == AT_ENTRY);
static_assert(cinux::loader::kAuxUid == AT_UID);
static_assert(cinux::loader::kAuxEuid == AT_EUID);
static_assert(cinux::loader::kAuxGid == AT_GID);
static_assert(cinux::loader::kAuxEgid == AT_EGID);
static_assert(cinux::loader::kAuxSecure == AT_SECURE);
static_assert(cinux::loader::kAuxRandom == AT_RANDOM);
static_assert(cinux::loader::kAuxExecfn == AT_EXECFN);
static_assert(cinux::loader::kAuxPlatform == AT_PLATFORM);
static_assert(cinux::loader::kAuxHwcap == AT_HWCAP);

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

constexpr std::size_t kInterpRunBytes = 24;
constexpr std::size_t kDynImageBytes  = kEhdrBytes + (2 * kPhdrBytes) + kInterpRunBytes;

struct DynImage {
    std::uint8_t bytes[kDynImageBytes] = {};
};

constexpr DynImage build_dyn_image() {
    DynImage image{};
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
    put_word(image, 56, 2);
    const std::size_t kFirst = kEhdrBytes;
    put_dword(image, kFirst, 1);
    put_dword(image, kFirst + 4, 5);
    put_qword(image, kFirst + 8, 0);
    put_qword(image, kFirst + 16, 0x400000);
    put_qword(image, kFirst + 24, 0x400000);
    put_qword(image, kFirst + 32, kImageBytes);
    put_qword(image, kFirst + 40, kImageBytes);
    put_qword(image, kFirst + 48, 0x1000);
    const std::size_t kSecond = kEhdrBytes + kPhdrBytes;
    const std::size_t kRunAt  = kSecond + kPhdrBytes;
    put_dword(image, kSecond, 3);
    put_dword(image, kSecond + 4, 4);
    put_qword(image, kSecond + 8, kRunAt);
    put_qword(image, kSecond + 32, 23);
    for (std::size_t index = 0; index < 23; ++index) {
        image.bytes[kRunAt + index] = static_cast<std::uint8_t>("/lib/ld-musl-x86_64.so.1"[index]);
    }
    return image;
}

constexpr DynImage kDynImage = build_dyn_image();

void check_good_segment(const cinux::loader::ElfImage& image) {
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(cinux::loader::ReadSegment(image, 0, segment));
    ASSERT_TRUE(segment.virtual_address == 0x401000);
    ASSERT_TRUE(segment.file_offset == kEhdrBytes + kPhdrBytes);
    ASSERT_TRUE(segment.file_bytes == kPayloadBytes);
    ASSERT_TRUE(segment.memory_bytes == 96);
    ASSERT_TRUE(segment.flags == (cinux::loader::kSegmentRead | cinux::loader::kSegmentExec));
}

TEST("elf: segment flags travel with the loadable facts") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                                   cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    TestImage writable = kGoodImage;
    put_dword(writable, kEhdrBytes + 4, 6);
    const auto kRewritten =
        cinux::loader::ParseElf64(writable.bytes, kImageBytes, cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kRewritten.ok());
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(cinux::loader::ReadSegment(kRewritten.value(), 0, segment));
    ASSERT_TRUE(segment.flags == (cinux::loader::kSegmentRead | cinux::loader::kSegmentWrite));
    ASSERT_TRUE(segment.virtual_address == 0x401000);
}

TEST("elf: a well-formed static image parses to entry and segments") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                                   cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(kParsed.value().entry == 0x401000);
    ASSERT_TRUE(kParsed.value().segment_count == 1);
    ASSERT_TRUE(kParsed.value().segment_size == 56);
    check_good_segment(kParsed.value());
}

void refuse_mangled_word(TestImage broken, std::size_t where, std::uint8_t value) {
    broken.bytes[where] = value;
    ASSERT_TRUE(
        !cinux::loader::ParseElf64(broken.bytes, kImageBytes, cinux::loader::ImageRole::kProgram)
             .ok());
}

TEST("elf: refusals name their family") {
    ASSERT_TRUE(!cinux::loader::ParseElf64(nullptr, 64, cinux::loader::ImageRole::kProgram).ok());
    ASSERT_TRUE(
        !cinux::loader::ParseElf64(kGoodImage.bytes, 32, cinux::loader::ImageRole::kProgram).ok());
    refuse_mangled_word(kGoodImage, 0, 'M');
    refuse_mangled_word(kGoodImage, 4, 1);
    refuse_mangled_word(kGoodImage, 18, 0x28);
    refuse_mangled_word(kGoodImage, 56, 0);
    refuse_mangled_word(kGoodImage, 54, 32);
}

TEST("elf: the program seat takes the relocatable shape and names its bias need") {
    TestImage relocatable = kGoodImage;
    relocatable.bytes[16] = cinux::loader::kTypeShared;
    const auto kParsed    = cinux::loader::ParseElf64(relocatable.bytes, kImageBytes,
                                                      cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(cinux::loader::ProgramNeedsLoadBias(relocatable.bytes, kImageBytes));
    ASSERT_TRUE(!cinux::loader::ProgramNeedsLoadBias(kGoodImage.bytes, kImageBytes));
}

TEST("elf: reads past the table refuse politely") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                                   cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(!cinux::loader::ReadSegment(kParsed.value(), 1, segment));
    ASSERT_TRUE(!cinux::loader::ReadSegment(kParsed.value(), 999, segment));
}

TEST("elf: the interpreter seat parses a shared image and reports its header table") {
    const auto kParsed = cinux::loader::ParseElf64(kSharedImage.bytes, kImageBytes,
                                                   cinux::loader::ImageRole::kInterpreter);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(kParsed.value().entry == 0x2000);
    ASSERT_TRUE(kParsed.value().headers_address == 64);
    cinux::loader::ElfSegment segment{};
    ASSERT_TRUE(cinux::loader::ReadSegment(kParsed.value(), 0, segment));
    ASSERT_TRUE(segment.virtual_address == 0);
    ASSERT_TRUE(segment.file_offset == 0);
    ASSERT_TRUE(segment.file_bytes == kImageBytes);
}

TEST("elf: the interpreter seat refuses the fixed-base shape") {
    ASSERT_TRUE(!cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                           cinux::loader::ImageRole::kInterpreter)
                     .ok());
}

TEST("elf: a table no loadable segment covers reports zero") {
    const auto kParsed = cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                                   cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(kParsed.value().headers_address == 0);
}

TEST("elf: an interpreter path reads out of the program's own bytes") {
    char path[64] = {};
    ASSERT_TRUE(
        cinux::loader::ReadInterpreterPath(kDynImage.bytes, kDynImageBytes, path, sizeof(path)));
    const char* const kWanted = "/lib/ld-musl-x86_64.so.1";
    for (std::size_t index = 0; index < 23; ++index) {
        ASSERT_TRUE(path[index] == kWanted[index]);
    }
    ASSERT_TRUE(path[23] == '\0');
    ASSERT_TRUE(
        !cinux::loader::ReadInterpreterPath(kGoodImage.bytes, kImageBytes, path, sizeof(path)));
    DynImage broken = kDynImage;
    put_qword(broken, kEhdrBytes + kPhdrBytes + 8, kDynImageBytes + 8);
    ASSERT_TRUE(
        !cinux::loader::ReadInterpreterPath(broken.bytes, kDynImageBytes, path, sizeof(path)));
}

constexpr std::size_t kPicturePageBytes = 4096;
constexpr std::size_t kPictureWordsAt   = cinux::proc::kUserEntryStackOffset;
constexpr std::size_t kPictureAuxvAt    = kPictureWordsAt + 40;

static_assert(kPictureWordsAt % 16 == 0);

struct ExpectedAuxv {
    unsigned long type;
    unsigned long value;
};

unsigned long picture_word(const unsigned char* page, std::size_t offset) {
    return cinux::base::ReadWord64(page, static_cast<std::uint32_t>(offset));
}

bool picture_find(const unsigned char* page, std::size_t words_at, unsigned long type,
                  unsigned long& value) {
    std::size_t walk = words_at;
    for (;;) {
        const unsigned long kType = picture_word(page, walk);
        if (kType == 0) {
            return false;
        }
        if (kType == type) {
            value = picture_word(page, walk + 8);
            return true;
        }
        walk += 16;
    }
}

std::size_t picture_pairs(const unsigned char* page, std::size_t words_at) {
    std::size_t count = 0;
    std::size_t walk  = words_at;
    while (picture_word(page, walk) != 0) {
        ++count;
        walk += 16;
    }
    return count;
}

void check_picture_entries(const unsigned char* page, std::size_t words_at,
                           const ExpectedAuxv* expected, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        unsigned long value = 1;
        ASSERT_TRUE(picture_find(page, words_at, expected[index].type, value));
        ASSERT_TRUE(value == expected[index].value);
    }
}

constexpr ExpectedAuxv kSharedFacts[] = {
    {.type = cinux::loader::kAuxPhdr, .value = 0x400040},
    {.type = cinux::loader::kAuxPhent, .value = 56},
    {.type = cinux::loader::kAuxPhnum, .value = 4},
    {.type = cinux::loader::kAuxPageSize, .value = 4096},
    {.type = cinux::loader::kAuxBase, .value = 0x10000000},
    {.type = cinux::loader::kAuxEntry, .value = 0x401000},
    {.type = cinux::loader::kAuxUid, .value = 0},
    {.type = cinux::loader::kAuxEuid, .value = 0},
    {.type = cinux::loader::kAuxGid, .value = 0},
    {.type = cinux::loader::kAuxEgid, .value = 0},
    {.type = cinux::loader::kAuxSecure, .value = 0},
    {.type = cinux::loader::kAuxExecfn, .value = 0x1FE6},
    {.type = cinux::loader::kAuxRandom, .value = 0x1FF0},
};

constexpr ExpectedAuxv kStaticFacts[] = {
    {.type = cinux::loader::kAuxPhdr, .value = 0x400040},
    {.type = cinux::loader::kAuxPhent, .value = 56},
    {.type = cinux::loader::kAuxPhnum, .value = 4},
    {.type = cinux::loader::kAuxPageSize, .value = 4096},
    {.type = cinux::loader::kAuxBase, .value = 0},
    {.type = cinux::loader::kAuxEntry, .value = 0x401000},
    {.type = cinux::loader::kAuxUid, .value = 0},
    {.type = cinux::loader::kAuxEuid, .value = 0},
    {.type = cinux::loader::kAuxGid, .value = 0},
    {.type = cinux::loader::kAuxEgid, .value = 0},
    {.type = cinux::loader::kAuxSecure, .value = 0},
    {.type = cinux::loader::kAuxExecfn, .value = 0x1FF6},
};

void check_picture_layout(const unsigned char* page) {
    ASSERT_TRUE(picture_word(page, kPictureWordsAt) == 1);
    ASSERT_TRUE(picture_word(page, kPictureWordsAt + 8) == 0x1FE6);
    ASSERT_TRUE(picture_word(page, kPictureWordsAt + 16) == 0);
    ASSERT_TRUE(picture_word(page, kPictureWordsAt + 24) == 0x1FDC);
    ASSERT_TRUE(picture_word(page, kPictureWordsAt + 32) == 0);
}

void check_picture_run(const unsigned char* page, std::size_t offset, const char* text) {
    std::size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    for (std::size_t index = 0; index < length; ++index) {
        ASSERT_TRUE(page[offset + index] == static_cast<std::uint8_t>(text[index]));
    }
}

void check_picture_random(const unsigned char* page, const std::uint8_t* source) {
    for (std::size_t index = 0; index < 16; ++index) {
        ASSERT_TRUE(page[4080 + index] == source[index]);
    }
}

void check_picture_platform(const unsigned char* page, std::size_t words_at,
                            unsigned long entry_rsp) {
    unsigned long hwcap_word = 1;
    ASSERT_TRUE(picture_find(page, words_at, cinux::loader::kAuxHwcap, hwcap_word));
    ASSERT_TRUE(hwcap_word == 0);
    unsigned long platform_at = 0;
    ASSERT_TRUE(picture_find(page, words_at, cinux::loader::kAuxPlatform, platform_at));
    ASSERT_TRUE(platform_at >= entry_rsp);
    const auto kTextAt = static_cast<std::size_t>(platform_at - 0x1000);
    ASSERT_TRUE(page[kTextAt] == 'x');
    ASSERT_TRUE(page[kTextAt + 5] == '4');
    ASSERT_TRUE(page[kTextAt + 6] == 0);
}

TEST("elf: the entry picture ends in the facts' auxiliary vector") {
    unsigned char     page[kPicturePageBytes] = {};
    const char* const kArgv[2]                = {"/bin/prog", nullptr};
    const char* const kEnvp[2]                = {"PATH=/bin", nullptr};
    std::uint8_t      random_source[16]       = {};
    for (std::size_t index = 0; index < 16; ++index) {
        random_source[index] = static_cast<std::uint8_t>(index + 1);
    }
    cinux::loader::EntryFacts facts{};
    facts.program_entry      = 0x401000;
    facts.program_headers    = 0x400040;
    facts.header_entry_bytes = 56;
    facts.header_count       = 4;
    facts.interpreter_base   = 0x10000000;
    facts.page_bytes         = 4096;
    facts.randomness         = random_source;
    facts.random_bytes       = 16;
    facts.platform           = "x86_64";
    const cinux::loader::EntryRuns kRuns{.arguments = kArgv, .environment = kEnvp};
    const unsigned long            kEntryRsp = 0x1000 + kPictureWordsAt;
    ASSERT_TRUE(cinux::loader::WriteEntryPicture(page, kEntryRsp, kRuns, facts) == kEntryRsp);
    check_picture_layout(page);
    check_picture_run(page, 4070, "/bin/prog");
    check_picture_random(page, random_source);
    check_picture_entries(page, kPictureAuxvAt, kSharedFacts, std::size(kSharedFacts));
    check_picture_platform(page, kPictureAuxvAt, kEntryRsp);
    ASSERT_TRUE(picture_pairs(page, kPictureAuxvAt) == 15);
    const std::size_t kNullValueAt = kPictureAuxvAt + (std::size_t{15} * 16) + 8;
    ASSERT_TRUE(picture_word(page, kNullValueAt) == 0);
}

TEST("elf: a static picture omits randomness and keeps the vector closed") {
    unsigned char             page[kPicturePageBytes] = {};
    const char* const         kArgv[2]                = {"/bin/prog", nullptr};
    cinux::loader::EntryFacts facts{};
    facts.program_entry      = 0x401000;
    facts.program_headers    = 0x400040;
    facts.header_entry_bytes = 56;
    facts.header_count       = 4;
    facts.page_bytes         = 4096;
    const cinux::loader::EntryRuns kRuns{.arguments = kArgv, .environment = nullptr};
    const unsigned long            kEntryRsp = 0x1000 + kPictureWordsAt;
    ASSERT_TRUE(cinux::loader::WriteEntryPicture(page, kEntryRsp, kRuns, facts) == kEntryRsp);
    check_picture_entries(page, kPictureWordsAt + 32, kStaticFacts, std::size(kStaticFacts));
    unsigned long value = 1;
    ASSERT_TRUE(!picture_find(page, kPictureWordsAt + 32, cinux::loader::kAuxRandom, value));
    ASSERT_TRUE(!picture_find(page, kPictureWordsAt + 32, cinux::loader::kAuxPlatform, value));
    ASSERT_TRUE(picture_find(page, kPictureWordsAt + 32, cinux::loader::kAuxHwcap, value));
    ASSERT_TRUE(value == 0);
    ASSERT_TRUE(picture_pairs(page, kPictureWordsAt + 32) == 13);
}


TEST("elf: every entry string lands above the initial stack pointer") {
    unsigned char             page[kPicturePageBytes] = {};
    const char* const         kArgv[4] = {"/bin/prog", "-v", "--long-flag=value", nullptr};
    const char* const         kEnvp[3] = {"PATH=/bin:/usr/bin", "HOME=/root", nullptr};
    cinux::loader::EntryFacts facts{};
    facts.program_entry      = 0x401000;
    facts.program_headers    = 0x400040;
    facts.header_entry_bytes = 56;
    facts.header_count       = 4;
    facts.page_bytes         = 4096;
    const cinux::loader::EntryRuns kRuns{.arguments = kArgv, .environment = kEnvp};
    const unsigned long            kEntryRsp = 0x1000 + kPictureWordsAt;
    ASSERT_TRUE(cinux::loader::WriteEntryPicture(page, kEntryRsp, kRuns, facts) == kEntryRsp);

    const char* const kStrings[5] = {
        "/bin/prog", "-v", "--long-flag=value", "PATH=/bin:/usr/bin", "HOME=/root",
    };
    const std::size_t kSeats[5]   = {8, 16, 24, 40, 48};
    unsigned long     pointers[5] = {};
    for (unsigned long which = 0; which < 5; ++which) {
        pointers[which] = picture_word(page, kPictureWordsAt + kSeats[which]);
        ASSERT_TRUE(pointers[which] >= kEntryRsp);
    }

    for (std::size_t index = 0; index < kPictureWordsAt; ++index) {
        page[index] = 0xAA;
    }
    for (unsigned long which = 0; which < 5; ++which) {
        const auto  kAt  = static_cast<std::size_t>(pointers[which] - 0x1000);
        std::size_t scan = 0;
        while (kStrings[which][scan] != '\0') {
            ASSERT_TRUE(page[kAt + scan] == static_cast<unsigned char>(kStrings[which][scan]));
            ++scan;
        }
        ASSERT_TRUE(page[kAt + scan] == 0);
    }
}

TEST("elf: strings exhausting the page are refused loudly") {
    unsigned char page[kPicturePageBytes] = {};
    static char   blob[59 * 130]          = {};
    const char*   argv_list[60]           = {};
    for (unsigned long index = 0; index < 59; ++index) {
        for (unsigned long fill = 0; fill < 129; ++fill) {
            blob[(index * 130) + fill] = static_cast<char>('a' + (index % 26));
        }
        blob[(index * 130) + 129] = '\0';
        argv_list[index]          = blob + (index * 130);
    }
    argv_list[59] = nullptr;

    cinux::loader::EntryFacts facts{};
    facts.program_entry      = 0x401000;
    facts.program_headers    = 0x400040;
    facts.header_entry_bytes = 56;
    facts.header_count       = 4;
    facts.page_bytes         = 4096;
    const cinux::loader::EntryRuns kRuns{.arguments = argv_list, .environment = nullptr};
    const unsigned long            kEntryRsp = 0x1000 + kPictureWordsAt;
    ASSERT_TRUE(cinux::loader::WriteEntryPicture(page, kEntryRsp, kRuns, facts) == 0);
}

constexpr TestImage build_gnu_stack_image(std::uint32_t stack_flags) {
    TestImage image = build_image();
    put_word(image, 56, 2);
    put_dword(image, kEhdrBytes + kPhdrBytes, 0x6474E551);
    put_dword(image, kEhdrBytes + kPhdrBytes + 4, stack_flags);
    return image;
}

TEST("elf: the gnu-stack header's flags read back, exec or not") {
    std::uint32_t flags = 0;
    ASSERT_TRUE(
        cinux::loader::ReadGnuStackFlags(build_gnu_stack_image(0x1).bytes, kImageBytes, flags));
    ASSERT_TRUE(flags == 0x1);
    ASSERT_TRUE(
        cinux::loader::ReadGnuStackFlags(build_gnu_stack_image(0x6).bytes, kImageBytes, flags));
    ASSERT_TRUE(flags == 0x6);
}

TEST("elf: images without a gnu-stack header answer absent") {
    std::uint32_t flags = 99;
    ASSERT_TRUE(!cinux::loader::ReadGnuStackFlags(kGoodImage.bytes, kImageBytes, flags));
    ASSERT_TRUE(flags == 99);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}

TEST("exec: argument budget follows RLIMIT_STACK floor ceiling and pointer accounting") {
    using cinux::loader::ArgumentStringBudget;
    const auto kDefault = ArgumentStringBudget(8UL * 1024 * 1024, 600, 400);
    ASSERT_TRUE(kDefault.ok());
    ASSERT_EQ(kDefault.value(), (2UL * 1024 * 1024) - (1000 * sizeof(unsigned long)));
    ASSERT_EQ(static_cast<unsigned long>(ArgumentStringBudget(4UL * 1024 * 1024, 1, 0).value()),
              (1024UL * 1024) - sizeof(unsigned long));
    ASSERT_EQ(static_cast<unsigned long>(ArgumentStringBudget(0, 0, 0).value()),
              (128UL * 1024) - sizeof(unsigned long));
    ASSERT_EQ(static_cast<unsigned long>(ArgumentStringBudget(~0ULL, 1, 0).value()),
              (6UL * 1024 * 1024) - sizeof(unsigned long));
    ASSERT_FALSE(ArgumentStringBudget(0, (128UL * 1024) / sizeof(unsigned long), 0).ok());
    ASSERT_FALSE(ArgumentStringBudget(~0ULL, cinux::loader::kMaxArgumentStrings + 1, 0).ok());
}

static void check_picture_guards(const std::vector<unsigned char>& bytes) {
    for (unsigned long index = 0; index < 16; ++index) {
        ASSERT_EQ(bytes[index], 0xA5U);
        ASSERT_EQ(bytes[bytes.size() - 1 - index], 0xA5U);
    }
}

TEST("exec: a measured multi-page picture preserves every string pointer and executable name") {
    const std::string              kText(8192, 'x');
    const char* const              kArguments[]   = {"different argv zero", kText.c_str(), nullptr};
    const char* const              kEnvironment[] = {"ENV=yes", nullptr};
    const cinux::loader::EntryRuns kRuns{.arguments = kArguments, .environment = kEnvironment};
    cinux::loader::EntryFacts      facts{};
    facts.executable                       = "/bin/actual";
    facts.platform                         = "x86_64";
    const auto                      kBytes = cinux::loader::EntryPictureBytes(kRuns, facts);
    std::vector<unsigned char>      bytes(kBytes + 32, 0xA5);
    constexpr unsigned long         kBase = 0x7FFF'0000;
    const cinux::loader::EntryStack kStack{
        .bytes    = bytes.data() + 16,
        .base     = kBase,
        .capacity = kBytes,
        .rsp      = kBase,
    };
    ASSERT_EQ(cinux::loader::WriteEntryPicture(kStack, kRuns, facts), kBase);
    ASSERT_EQ(cinux::base::ReadWord64(bytes.data(), 16), 2ULL);
    const auto kFirst  = cinux::base::ReadWord64(bytes.data(), 24) - kBase;
    const auto kSecond = cinux::base::ReadWord64(bytes.data(), 32) - kBase;
    ASSERT_EQ(std::string(reinterpret_cast<char*>(bytes.data() + 16 + kFirst)),
              "different argv zero");
    ASSERT_EQ(std::string(reinterpret_cast<char*>(bytes.data() + 16 + kSecond)).compare(kText), 0);
    unsigned long executable = 0;
    ASSERT_TRUE(picture_find(bytes.data() + 16, 6 * sizeof(unsigned long),
                             cinux::loader::kAuxExecfn, executable));
    ASSERT_EQ(std::string(reinterpret_cast<char*>(bytes.data() + 16 + executable - kBase)),
              "/bin/actual");
    check_picture_guards(bytes);
    ASSERT_EQ(cinux::loader::WriteEntryPicture(
                  {.bytes = bytes.data(), .base = kBase, .capacity = kBytes - 1, .rsp = kBase},
                  kRuns, facts),
              0UL);
}
