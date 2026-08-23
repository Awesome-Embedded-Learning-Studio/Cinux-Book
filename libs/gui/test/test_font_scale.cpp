/**
 * @file test/test_font_scale.cpp
 * @brief P5-a -- integer font scaling + text measurement
 *
 * Asserts:
 *   - text_width / text_height handle single + multi-line at scale 1 and 2
 *   - glyph_blit_scaled at scale 2 paints exactly 4x the pixels of scale 1
 *     (each set bit -> a 2x2 block, nearest neighbour, no overlap)
 *   - a kTextScaled PaintList cmd rendered via execute() matches the direct
 *     glyph_blit_scaled pixel count
 *
 * Standalone ctest. Pure logic (paints into a malloc staging buffer).
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "compositor.hpp"  // execute
#include "font.hpp"        // PsfFont, text_width/height
#include "paint_list.hpp"  // PaintList
#include "swraster.hpp"    // Surface, glyph_blit(_scaled)

using namespace cinux::gui;

static uint32_t count_white(const uint8_t* buf, uint32_t w, uint32_t h) {
    uint32_t        n = 0;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(buf);
    for (uint32_t i = 0; i < w * h; ++i) {
        if (p[i] == 0x00FFFFFFu) {
            ++n;
        }
    }
    return n;
}

int main() {
    PsfFont font;
    font.init();
    assert(font.ready());
    constexpr uint32_t kW = 48;
    constexpr uint32_t kH = 48;

    /* --- 1. text_width / text_height (single + multi-line, scale 1 & 2) --- */
    assert(text_width(font, "AB", 1) == 2u * font.width());
    assert(text_width(font, "AB", 2) == 4u * font.width());
    assert(text_height(font, "AB", 1) == font.height());
    assert(text_height(font, "AB", 2) == 2u * font.height());
    assert(text_height(font, "A\nB", 1) == 2u * font.height());  // two lines
    assert(text_width(font, "A\nBB", 1) == 2u * font.width());   // longest line "BB"

    /* --- 2. glyph_blit_scaled: scale 2 == 4x the scale-1 set pixels --- */
    const uint8_t* gA = font.glyph('A');
    assert(gA != nullptr);

    uint8_t b1[kW * kH * 4u] = {};
    Surface s1{b1, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    glyph_blit(s1, 0, 0, gA, font.width(), font.height(), 0x00FFFFFFu, nullptr);
    const uint32_t n1 = count_white(b1, kW, kH);

    uint8_t b2[kW * kH * 4u] = {};
    Surface s2{b2, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    glyph_blit_scaled(s2, 0, 0, gA, font.width(), font.height(), 2, 0x00FFFFFFu, nullptr);
    const uint32_t n2 = count_white(b2, kW, kH);

    assert(n1 > 0);
    assert(n2 == n1 * 4u);  // each bit -> 2x2 block

    /* --- 3. kTextScaled via PaintList + execute == direct glyph_blit_scaled --- */
    uint8_t   b3[kW * kH * 4u] = {};
    Surface   s3{b3, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    PaintList list;
    list.text_scaled(0, 0, 0x00FFFFFFu, "A", 2);
    {
        Compositor comp;  // P6-d: Compositor is a class now
        comp.render(s3, list, font);
    }
    assert(count_white(b3, kW, kH) == n2);

    std::printf("test_font_scale: OK\n");
    return 0;
}
