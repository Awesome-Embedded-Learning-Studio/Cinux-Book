#include <stdint.h>

#include "../framework/framework.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/driver/framebuffer.hpp"
#include "test_assert.hpp"

using cinux::boot::FramebufferInfo;
using cinux::driver::ComposePixel;
using cinux::driver::kInk;
using cinux::driver::kPaper;
using cinux::driver::Paint;
using cinux::driver::PixelByteOffset;

namespace {

constexpr FramebufferInfo kXrgbLayout{
    .physical    = 0xFD000000,
    .pitch       = 4096,
    .width       = 1024,
    .height      = 768,
    .bpp         = 32,
    .red_size    = 8,
    .red_shift   = 16,
    .green_size  = 8,
    .green_shift = 8,
    .blue_size   = 8,
    .blue_shift  = 0,
};

constexpr Paint kHalfRed{.red = 0xFF, .green = 0x80, .blue = 0x00};

}  // namespace

TEST("framebuffer: XRGB layout composes white and black") {
    ASSERT_TRUE(ComposePixel(kXrgbLayout, kInk) == 0x00FFFFFF);
    ASSERT_TRUE(ComposePixel(kXrgbLayout, kPaper) == 0x00000000);
}

TEST("framebuffer: channels land in their own fields") {
    uint32_t const kPixel = ComposePixel(kXrgbLayout, kHalfRed);
    ASSERT_TRUE(kPixel == ((0xFFU << 16) | (0x80U << 8) | 0x00U));
}

TEST("framebuffer: masks truncate wide paints") {
    FramebufferInfo six_bit = kXrgbLayout;
    six_bit.red_size        = 6;
    six_bit.red_shift       = 10;
    uint32_t const kPixel   = ComposePixel(six_bit, kInk);
    ASSERT_TRUE(((kPixel >> 10) & 0x3FU) == 0x3FU);
}

TEST("framebuffer: pixel offsets follow pitch not width") {
    ASSERT_TRUE(PixelByteOffset(4096, 0, 0) == 0);
    ASSERT_TRUE(PixelByteOffset(4096, 1, 0) == 4);
    ASSERT_TRUE(PixelByteOffset(4096, 0, 1) == 4096);
    ASSERT_TRUE(PixelByteOffset(4096, 1023, 767) == (4096ULL * 767U) + 4092U);
}

int main() {
    return cinux::test::RunAll();
}
