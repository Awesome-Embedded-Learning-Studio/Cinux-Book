#include "../framework/framework.hpp"
#include "boot/vesa/vesa.hpp"

using cinux::boot::MakeSample;
using cinux::boot::MatchesRequest;

TEST("vesa: constexpr matching agrees at runtime") {
    auto const kLfb32 = MakeSample({.attributes  = 0x0080,
                                    .pitch       = 4096,
                                    .width       = 1024,
                                    .height      = 768,
                                    .bpp         = 32,
                                    .framebuffer = 0xFD000000});
    ASSERT_TRUE(MatchesRequest(kLfb32, 1024, 768, 32));
    ASSERT_TRUE(!MatchesRequest(kLfb32, 1024, 768, 24));
    ASSERT_TRUE(!MatchesRequest(MakeSample({.attributes  = 0x000A,
                                            .pitch       = 3072,
                                            .width       = 1024,
                                            .height      = 768,
                                            .bpp         = 24,
                                            .framebuffer = 0xFD000000}),
                                1024, 768, 24));
    ASSERT_TRUE(!MatchesRequest(MakeSample({.attributes  = 0x0090,
                                            .pitch       = 3072,
                                            .width       = 1024,
                                            .height      = 768,
                                            .bpp         = 24,
                                            .framebuffer = 0}),
                                1024, 768, 24));
}

TEST("vesa: framebuffer archive is host-constructible") {
    cinux::boot::FrameBufferInfo archive{};
    archive.physical = 0xFD000000ULL;
    archive.pitch    = 4096;
    archive.width    = 1024;
    archive.height   = 768;
    archive.bpp      = 32;
    ASSERT_TRUE(archive.width == 1024);
    ASSERT_TRUE(archive.bpp == 32);

    cinux::boot::VbeInfoBlock  ctrl{};
    cinux::boot::ModeInfoBlock mode{};
    ctrl.version = 0x0300;
    mode.width   = 1024;
    ASSERT_TRUE(ctrl.version == 0x0300);
    ASSERT_TRUE(MatchesRequest(mode, 1024, 768, 32) == false);
}

TEST("vesa: vbe info block pins") {
    ASSERT_TRUE(sizeof(cinux::boot::VbeInfoBlock) == 512);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::VbeInfoBlock, version) == 0x04);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::VbeInfoBlock, mode_list_offset) == 0x0E);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::VbeInfoBlock, total_memory) == 0x12);
}

TEST("vesa: mode info block pins") {
    ASSERT_TRUE(sizeof(cinux::boot::ModeInfoBlock) == 256);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::ModeInfoBlock, pitch) == 0x10);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::ModeInfoBlock, width) == 0x12);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::ModeInfoBlock, height) == 0x14);
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::ModeInfoBlock, bpp) == 0x19);
}

TEST("vesa: mode block tail offsets") {
    ASSERT_TRUE(__builtin_offsetof(cinux::boot::ModeInfoBlock, framebuffer) == 0x28);
}

TEST("vesa: migrated fb and matches pins") {
    ASSERT_TRUE(sizeof(cinux::boot::FrameBufferInfo) == 17);
}

TEST("vesa: migrated matches pins") {
    ASSERT_TRUE(cinux::boot::MatchesRequest(cinux::boot::MakeSample({.attributes  = 0x0080,
                                                                     .pitch       = 4096,
                                                                     .width       = 1024,
                                                                     .height      = 768,
                                                                     .bpp         = 32,
                                                                     .framebuffer = 0xFD000000}),
                                            1024, 768, 32));
    ASSERT_TRUE(!cinux::boot::MatchesRequest(cinux::boot::MakeSample({.attributes  = 0x0080,
                                                                      .pitch       = 4096,
                                                                      .width       = 1024,
                                                                      .height      = 768,
                                                                      .bpp         = 32,
                                                                      .framebuffer = 0xFD000000}),
                                             1024, 768, 24));
}

TEST("vesa: rejected modes pins") {
    ASSERT_TRUE(!cinux::boot::MatchesRequest(cinux::boot::MakeSample({.attributes  = 0x0090,
                                                                      .pitch       = 3072,
                                                                      .width       = 1024,
                                                                      .height      = 768,
                                                                      .bpp         = 24,
                                                                      .framebuffer = 0}),
                                             1024, 768, 24));
}

int main() {
    return cinux::test::RunAll();
}
