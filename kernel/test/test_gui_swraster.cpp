/**
 * @file kernel/test/test_gui_swraster.cpp
 * @brief QEMU in-kernel tests for the cinux::gui SwRaster primitives (F13 §4a)
 *
 * Pure-logic tests: each builds a small stack Surface, runs a primitive, and
 * asserts exact pixel values. SwRaster is not wired into pump yet (§4a
 * is the shape skeleton); these tests are its only consumer until §4c.
 *
 * Compile condition: CINUX_GUI (swraster lives under kernel/gui).
 */

#include <stdint.h>

#include "big_kernel_test.h"

#ifdef CINUX_GUI
#    include "third_party/Cinux-GUI/core/swraster.hpp"
#endif

#ifdef CINUX_GUI

using cinux::gui::ClipRect;
using cinux::gui::Surface;
using cinux::gui::PixelFormat;

/* Build a tight (stride == width*4) XRGB8888 surface over a stack buffer. */
static Surface make_surface(uint32_t* buf, uint32_t w, uint32_t h) {
    return Surface{buf, w, h, w * 4u, PixelFormat::kXrgb8888};
}

namespace test_gui_fill {
void test_fill_basic() {
    uint32_t buf[16] = {0};
    Surface  s       = make_surface(buf, 4, 4);
    cinux::gui::fill_rect(s, 1, 1, 2, 2, 0x00FF0000u, nullptr);
    TEST_ASSERT_EQ(buf[1 * 4 + 1], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[1 * 4 + 2], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[2 * 4 + 1], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[2 * 4 + 2], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[0], 0u); /* untouched corner */
}

void test_fill_clip_surface() {
    /* Fill a 6x6 region on a 4x4 surface: only the [0,4)x[0,4) overlap writes. */
    uint32_t buf[16] = {0};
    Surface  s       = make_surface(buf, 4, 4);
    cinux::gui::fill_rect(s, 0, 0, 6, 6, 0x0000FF00u, nullptr);
    for (uint32_t i = 0; i < 16; i++) {
        TEST_ASSERT_EQ(buf[i], 0x0000FF00u);
    }
}

void test_fill_clip_rect() {
    uint32_t buf[16] = {0};
    Surface  s       = make_surface(buf, 4, 4);
    ClipRect clip{0, 0, 2, 2}; /* only top-left 2x2 writable */
    cinux::gui::fill_rect(s, 0, 0, 4, 4, 0x000000FFu, &clip);
    TEST_ASSERT_EQ(buf[0 * 4 + 0], 0x000000FFu);
    TEST_ASSERT_EQ(buf[0 * 4 + 1], 0x000000FFu);
    TEST_ASSERT_EQ(buf[1 * 4 + 0], 0x000000FFu);
    TEST_ASSERT_EQ(buf[1 * 4 + 1], 0x000000FFu);
    TEST_ASSERT_EQ(buf[3 * 4 + 3], 0u); /* outside clip, untouched */
}
}  // namespace test_gui_fill

namespace test_gui_blit {
void test_blit_basic() {
    uint32_t src_buf[4]  = {0x00111111u, 0x00222222u, 0x00333333u, 0x00444444u};
    uint32_t dst_buf[16] = {0};
    Surface  src         = make_surface(src_buf, 2, 2);
    Surface  dst         = make_surface(dst_buf, 4, 4);
    cinux::gui::blit(dst, 1, 1, src, 0, 0, 2, 2, nullptr);
    TEST_ASSERT_EQ(dst_buf[1 * 4 + 1], 0x00111111u);
    TEST_ASSERT_EQ(dst_buf[1 * 4 + 2], 0x00222222u);
    TEST_ASSERT_EQ(dst_buf[2 * 4 + 1], 0x00333333u);
    TEST_ASSERT_EQ(dst_buf[2 * 4 + 2], 0x00444444u);
}

void test_blit_partial_clip() {
    /* Blit a 4x4 source to dst@(2,2) on a 4x4 dst: only [2,4)x[2,4) lands. */
    uint32_t src_buf[16];
    for (uint32_t i = 0; i < 16; i++) {
        src_buf[i] = 0x00AA0000u + i;
    }
    uint32_t dst_buf[16] = {0};
    Surface  src         = make_surface(src_buf, 4, 4);
    Surface  dst         = make_surface(dst_buf, 4, 4);
    cinux::gui::blit(dst, 2, 2, src, 0, 0, 4, 4, nullptr);
    /* src(0,0)->dst(2,2), src(1,0)->dst(3,2), src(0,1)->dst(2,3), src(1,1)->dst(3,3) */
    TEST_ASSERT_EQ(dst_buf[2 * 4 + 2], src_buf[0 * 4 + 0]);
    TEST_ASSERT_EQ(dst_buf[2 * 4 + 3], src_buf[0 * 4 + 1]);
    TEST_ASSERT_EQ(dst_buf[3 * 4 + 2], src_buf[1 * 4 + 0]);
    TEST_ASSERT_EQ(dst_buf[3 * 4 + 3], src_buf[1 * 4 + 1]);
    TEST_ASSERT_EQ(dst_buf[0], 0u); /* untouched */
}
}  // namespace test_gui_blit

namespace test_gui_blend {
void test_blend_zero() {
    uint32_t src_buf[4] = {0x00FFFFFFu, 0x00FFFFFFu, 0x00FFFFFFu, 0x00FFFFFFu};
    uint32_t dst_buf[4] = {0x00000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu};
    Surface  src        = make_surface(src_buf, 2, 2);
    Surface  dst        = make_surface(dst_buf, 2, 2);
    cinux::gui::blit_blend(dst, 0, 0, src, 0, 0, 2, 2, 0, nullptr); /* a=0 -> dst unchanged */
    TEST_ASSERT_EQ(dst_buf[0], 0x00000000u);
    TEST_ASSERT_EQ(dst_buf[1], 0x00FF0000u);
    TEST_ASSERT_EQ(dst_buf[2], 0x0000FF00u);
    TEST_ASSERT_EQ(dst_buf[3], 0x000000FFu);
}

void test_blend_full() {
    uint32_t src_buf[1] = {0x00ABCDEFu};
    uint32_t dst_buf[1] = {0x00000000u};
    Surface  src        = make_surface(src_buf, 1, 1);
    Surface  dst        = make_surface(dst_buf, 1, 1);
    cinux::gui::blit_blend(dst, 0, 0, src, 0, 0, 1, 1, 256, nullptr); /* a=256 -> src */
    TEST_ASSERT_EQ(dst_buf[0], 0x00ABCDEFu);
}

void test_blend_half() {
    /* src=0x00FF0000 (R=255), dst=0x000000FF (B=255), a=128 -> R=127,B=127. */
    uint32_t src_buf[1] = {0x00FF0000u};
    uint32_t dst_buf[1] = {0x000000FFu};
    Surface  src        = make_surface(src_buf, 1, 1);
    Surface  dst        = make_surface(dst_buf, 1, 1);
    cinux::gui::blit_blend(dst, 0, 0, src, 0, 0, 1, 1, 128, nullptr);
    const uint32_t r = (dst_buf[0] >> 16) & 0xFFu;
    const uint32_t b = dst_buf[0] & 0xFFu;
    TEST_ASSERT_EQ(r, 127u);
    TEST_ASSERT_EQ(b, 127u);
}
}  // namespace test_gui_blend

namespace test_gui_glyph {
void test_glyph_mask() {
    /* 8x2 glyph, MSB-first per row:
     *   row0 = 0b10101010 = 0xAA (cols 0,2,4,6 set)
     *   row1 = 0b00001111 = 0x0F (cols 4,5,6,7 set) */
    const uint8_t bits[2] = {0xAAu, 0x0Fu};
    uint32_t      buf[16] = {0};
    Surface       s       = make_surface(buf, 8, 2);
    cinux::gui::glyph_blit(s, 0, 0, bits, 8, 2, 0x00FFFFFFu, nullptr);
    /* row0: cols 0,2,4,6 set */
    TEST_ASSERT_EQ(buf[0 * 8 + 0], 0x00FFFFFFu);
    TEST_ASSERT_EQ(buf[0 * 8 + 1], 0u);
    TEST_ASSERT_EQ(buf[0 * 8 + 2], 0x00FFFFFFu);
    TEST_ASSERT_EQ(buf[0 * 8 + 6], 0x00FFFFFFu);
    TEST_ASSERT_EQ(buf[0 * 8 + 7], 0u);
    /* row1: cols 4-7 set */
    TEST_ASSERT_EQ(buf[1 * 8 + 3], 0u);
    TEST_ASSERT_EQ(buf[1 * 8 + 4], 0x00FFFFFFu);
    TEST_ASSERT_EQ(buf[1 * 8 + 7], 0x00FFFFFFu);
}
}  // namespace test_gui_glyph

namespace test_gui_line {
void test_line_horizontal() {
    uint32_t buf[16] = {0};
    Surface  s       = make_surface(buf, 4, 4);
    cinux::gui::draw_line(s, 0, 1, 3, 1, 0x00FF0000u, nullptr);
    TEST_ASSERT_EQ(buf[1 * 4 + 0], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[1 * 4 + 1], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[1 * 4 + 2], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[1 * 4 + 3], 0x00FF0000u);
    TEST_ASSERT_EQ(buf[0 * 4 + 0], 0u);
}

void test_line_clip() {
    /* Line crosses surface edge; out-of-bounds points are skipped. */
    uint32_t buf[16] = {0};
    Surface  s       = make_surface(buf, 4, 4);
    cinux::gui::draw_line(s, -2, 0, 1, 0, 0x0000FF00u, nullptr);
    TEST_ASSERT_EQ(buf[0 * 4 + 0], 0x0000FF00u);
    TEST_ASSERT_EQ(buf[0 * 4 + 1], 0x0000FF00u);
}
}  // namespace test_gui_line

namespace test_gui_stride {
void test_stride_padding() {
    /* width=4 but stride=24 bytes (6 pixels/row): 2 padding columns per row.
     * fill_rect must index via stride, not width, and not overrun. */
    uint32_t buf[6 * 4] = {0}; /* 6 pixels/row * 4 rows */
    Surface  s{buf, 4, 4, 24u, PixelFormat::kXrgb8888};
    cinux::gui::fill_rect(s, 0, 0, 4, 4, 0x00BBCCDDu, nullptr);
    /* Every logical pixel filled; padding columns untouched. */
    for (uint32_t r = 0; r < 4; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            TEST_ASSERT_EQ(buf[r * 6 + c], 0x00BBCCDDu);
        }
        TEST_ASSERT_EQ(buf[r * 6 + 4], 0u); /* padding */
        TEST_ASSERT_EQ(buf[r * 6 + 5], 0u);
    }
}
}  // namespace test_gui_stride

extern "C" void run_gui_swraster_tests() {
    TEST_SECTION("cinux::gui SwRaster Tests (F13 §4a)");
    RUN_TEST(test_gui_fill::test_fill_basic);
    RUN_TEST(test_gui_fill::test_fill_clip_surface);
    RUN_TEST(test_gui_fill::test_fill_clip_rect);
    RUN_TEST(test_gui_blit::test_blit_basic);
    RUN_TEST(test_gui_blit::test_blit_partial_clip);
    RUN_TEST(test_gui_blend::test_blend_zero);
    RUN_TEST(test_gui_blend::test_blend_full);
    RUN_TEST(test_gui_blend::test_blend_half);
    RUN_TEST(test_gui_glyph::test_glyph_mask);
    RUN_TEST(test_gui_line::test_line_horizontal);
    RUN_TEST(test_gui_line::test_line_clip);
    RUN_TEST(test_gui_stride::test_stride_padding);
    TEST_SUMMARY();
}

#else /* !CINUX_GUI */

/* Non-GUI build: SwRaster is not compiled, expose an empty runner so the
 * main_test forward declaration links. main_test only calls it under GUI. */
extern "C" void run_gui_swraster_tests() {}

#endif /* CINUX_GUI */
