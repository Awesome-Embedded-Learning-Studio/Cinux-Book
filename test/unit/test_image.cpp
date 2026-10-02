#include "../framework/framework.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/image_header.hpp"

using cinux::boot::BootInfo;
using cinux::boot::E820Entry;
using cinux::boot::FramebufferInfo;
using cinux::boot::ImageHeader;
using cinux::boot::LoadStatus;
using cinux::boot::NameOf;
using cinux::boot::Region;
using cinux::boot::ValidateImage;

namespace {

constexpr Region kUsable[] = {{.base = 0, .top = 0x9FC00}, {.base = 0x100000, .top = 0x7FE0000}};

ImageHeader valid_header() {
    return {.magic       = cinux::boot::kImageMagic,
            .version     = cinux::boot::kImageVersion,
            .header_size = sizeof(ImageHeader),
            .load_paddr  = 0x200000,
            .file_size   = 0x1000,
            .mem_size    = 0x2000,
            .entry       = 0x200100};
}

LoadStatus verdict(const ImageHeader& header) {
    return ValidateImage(header, kUsable, 2, nullptr, 0);
}

}  // namespace

TEST("image: header layout pins") {
    ASSERT_TRUE(sizeof(ImageHeader) == 40);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, magic) == 0);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, version) == 4);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, header_size) == 6);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, load_paddr) == 8);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, file_size) == 16);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, mem_size) == 24);
    ASSERT_TRUE(__builtin_offsetof(ImageHeader, entry) == 32);
}

TEST("image: bootinfo layout pins") {
    ASSERT_TRUE(sizeof(E820Entry) == 20);
    ASSERT_TRUE(sizeof(FramebufferInfo) == 48);
    ASSERT_TRUE(sizeof(BootInfo) == 2656);
    ASSERT_TRUE(cinux::boot::kBootInfoE820Max == 128);
    ASSERT_TRUE(__builtin_offsetof(BootInfo, e820) == 16);
    ASSERT_TRUE(__builtin_offsetof(BootInfo, framebuffer) == 2576);
    ASSERT_TRUE(__builtin_offsetof(BootInfo, kernel_paddr) == 2624);
    ASSERT_TRUE(__builtin_offsetof(BootInfo, kernel_entry) == 2648);
}

TEST("image: contract constants are pinned") {
    ASSERT_TRUE(cinux::boot::kImageMagic == 0x5A4B4E43);
    ASSERT_TRUE(cinux::boot::kImageVersion == 1);
    ASSERT_TRUE(cinux::boot::kHandoffDoorTop == 0x40000000ULL);
    ASSERT_TRUE(cinux::boot::kBootInfoMagic == 0x00114514);
    ASSERT_TRUE(cinux::boot::kBootInfoVersion == 1);
}

TEST("image: valid header passes all nine checks") {
    ASSERT_TRUE(verdict(valid_header()) == LoadStatus::kOk);
    ImageHeader at_base = valid_header();
    at_base.entry       = at_base.load_paddr;
    ASSERT_TRUE(verdict(at_base) == LoadStatus::kOk);
    ImageHeader at_top = valid_header();
    at_top.entry       = at_top.load_paddr + at_top.mem_size - 1;
    ASSERT_TRUE(verdict(at_top) == LoadStatus::kOk);
    ASSERT_STREQ(NameOf(LoadStatus::kOk), "ok");
}

TEST("image: header clause rejects") {
    ImageHeader bad_magic = valid_header();
    bad_magic.magic       = 0;
    ASSERT_TRUE(verdict(bad_magic) == LoadStatus::kBadMagic);

    ImageHeader bad_version = valid_header();
    bad_version.version     = 2;
    ASSERT_TRUE(verdict(bad_version) == LoadStatus::kBadVersion);

    ImageHeader empty = valid_header();
    empty.file_size   = 0;
    ASSERT_TRUE(verdict(empty) == LoadStatus::kEmptyImage);

    ImageHeader bad_sizes = valid_header();
    bad_sizes.mem_size    = bad_sizes.file_size - 1;
    ASSERT_TRUE(verdict(bad_sizes) == LoadStatus::kBadSizes);
}

TEST("image: address clause rejects") {
    ImageHeader wraps = valid_header();
    wraps.load_paddr  = 0xFFFFFFFFFFFFFFFFULL;
    wraps.mem_size    = 2;
    wraps.file_size   = 1;
    ASSERT_TRUE(verdict(wraps) == LoadStatus::kPaddrOverflow);

    ImageHeader fat_file = valid_header();
    fat_file.load_paddr  = 0;
    fat_file.file_size   = 0xFFFFFFFFFFFFFFFFULL;
    fat_file.mem_size    = 0xFFFFFFFFFFFFFFFFULL;
    ASSERT_TRUE(verdict(fat_file) == LoadStatus::kPaddrOverflow);

    ImageHeader one_past_door = valid_header();
    one_past_door.load_paddr  = 0x3FFFF000ULL;
    one_past_door.file_size   = 0x1000;
    one_past_door.mem_size    = 0x1001;
    one_past_door.entry       = 0x3FFFF100;
    ASSERT_TRUE(verdict(one_past_door) == LoadStatus::kBeyondDoors);

    ImageHeader way_past_4g = valid_header();
    way_past_4g.load_paddr  = 0x100000000ULL;
    way_past_4g.entry       = 0x100000100ULL;
    ASSERT_TRUE(verdict(way_past_4g) == LoadStatus::kBeyondDoors);

    ImageHeader entry_low = valid_header();
    entry_low.entry       = 0x1FFFFF;
    ASSERT_TRUE(verdict(entry_low) == LoadStatus::kEntryOutside);

    ImageHeader entry_high = valid_header();
    entry_high.entry       = entry_high.load_paddr + entry_high.mem_size;
    ASSERT_TRUE(verdict(entry_high) == LoadStatus::kEntryOutside);
}

TEST("image: door ceiling edge is inclusive") {
    constexpr Region kReach1g[] = {{.base = 0x100000, .top = 0x40000000ULL}};
    ImageHeader      at_ceiling = valid_header();
    at_ceiling.load_paddr       = 0x3FFFF000ULL;
    at_ceiling.file_size        = 0x1000;
    at_ceiling.mem_size         = 0x1000;
    at_ceiling.entry            = 0x3FFFF800;
    ASSERT_TRUE(ValidateImage(at_ceiling, kReach1g, 1, nullptr, 0) == LoadStatus::kOk);
}

TEST("image: memory reality rejects") {
    ImageHeader spans_hole = valid_header();
    spans_hole.load_paddr  = 0x9F000;
    spans_hole.mem_size    = 0x20000;
    spans_hole.entry       = 0x9F100;
    ASSERT_TRUE(verdict(spans_hole) == LoadStatus::kNotInUsable);

    ImageHeader in_hole = valid_header();
    in_hole.load_paddr  = 0xA0000;
    in_hole.mem_size    = 0x10000;
    in_hole.entry       = 0xA0100;
    ASSERT_TRUE(verdict(in_hole) == LoadStatus::kNotInUsable);

    constexpr Region kOwned[] = {{.base = 0x201000, .top = 0x201800}};
    ASSERT_TRUE(ValidateImage(valid_header(), kUsable, 2, kOwned, 1) == LoadStatus::kBootOverlap);

    constexpr Region kTouchAbove[] = {{.base = 0x202000, .top = 0x203000}};
    ASSERT_TRUE(ValidateImage(valid_header(), kUsable, 2, kTouchAbove, 1) == LoadStatus::kOk);

    constexpr Region kTouchBelow[] = {{.base = 0x1FF000, .top = 0x200000}};
    ASSERT_TRUE(ValidateImage(valid_header(), kUsable, 2, kTouchBelow, 1) == LoadStatus::kOk);
}

TEST("image: cheapest check wins") {
    ImageHeader broken = valid_header();
    broken.magic       = 0;
    broken.version     = 99;
    broken.file_size   = 0;
    ASSERT_TRUE(verdict(broken) == LoadStatus::kBadMagic);

    ImageHeader no_regions = valid_header();
    ASSERT_TRUE(ValidateImage(no_regions, nullptr, 0, nullptr, 0) == LoadStatus::kNotInUsable);
}

TEST("image: ferry runtime statuses stay nameable") {
    ASSERT_STREQ(NameOf(LoadStatus::kDiskError), "disk read error");
    ASSERT_STREQ(NameOf(LoadStatus::kMagicMismatch), "delivery check failed");
}

int main() {
    return cinux::test::RunAll();
}
