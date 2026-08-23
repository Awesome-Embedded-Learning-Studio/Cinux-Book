/**
 * @file test/test_cursor.cpp
 * @brief P7-c Compositor cursor state -- set_cursor + render paints a block
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "compositor.hpp"  // Compositor
#include "font.hpp"        // PsfFont
#include "paint_list.hpp"  // PaintList
#include "swraster.hpp"    // Surface

using namespace cinux::gui;

int main() {
    PsfFont font;
    font.init();
    constexpr uint32_t kW = 32;
    constexpr uint32_t kH = 32;

    /* cursor visible -> a 4x4 white block lands at (10,10) */
    uint8_t    b[kW * kH * 4u] = {};
    Surface    s{b, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    Compositor comp;
    comp.set_cursor(10, 10, true);
    PaintList list;  // empty widget list -- only the cursor should paint
    comp.render(s, list, font);
    const uint32_t* px = reinterpret_cast<const uint32_t*>(b);
    assert(px[10u * kW + 10u] == 0x00FFFFFFu);

    /* cursor hidden -> no white pixel */
    uint8_t    b2[kW * kH * 4u] = {};
    Surface    s2{b2, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    Compositor comp2;
    comp2.set_cursor(10, 10, false);
    comp2.render(s2, list, font);
    const uint32_t* px2 = reinterpret_cast<const uint32_t*>(b2);
    assert(px2[10u * kW + 10u] != 0x00FFFFFFu);

    std::printf("test_cursor: OK\n");
    return 0;
}
