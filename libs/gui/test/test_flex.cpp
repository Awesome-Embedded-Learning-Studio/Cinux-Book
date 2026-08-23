/**
 * @file test/test_flex.cpp
 * @brief P5-d -- HBox/VBox flex weights + per-corner rounded rects
 *
 * Asserts:
 *   - HBox splits width by flex weight (3:1 -> 75/25); last child gets remainder
 *   - default flex 1 each = equal share (compat with P3-c)
 *   - fill_rounded_rect with kCornerTL|kCornerTR rounds the top corners only:
 *     top corner stays bg (arc bites), bottom corner is painted (square)
 *
 * Standalone ctest.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "paint_list.hpp"        // (ensures cmd types link)
#include "swraster.hpp"          // fill_rounded_rect, kCorner*
#include "widget.hpp"            // Widget
#include "widget/container.hpp"  // HBox
#include "widget/label.hpp"      // Label

using namespace cinux::gui;

int main() {
    /* --- 1. flex: HBox 3:1 split (a=75, b=25) --- */
    {
        HBox hbox;
        hbox.set_rect(0, 0, 100, 20);
        Label a;
        Label b;
        a.set_flex(3);
        b.set_flex(1);
        hbox.add_child(&a);
        hbox.add_child(&b);
        hbox.layout();
        assert(a.rect().width() == 75u);  // 100 * 3/4
        assert(b.rect().width() == 25u);  // remainder
    }

    /* --- 2. default flex 1 each = equal share (3 children of 100 -> 33/33/34) --- */
    {
        HBox hbox;
        hbox.set_rect(0, 0, 100, 20);
        Label a;
        Label b;
        Label c;
        hbox.add_child(&a);
        hbox.add_child(&b);
        hbox.add_child(&c);
        hbox.layout();
        assert(a.rect().width() == 33u);
        assert(b.rect().width() == 33u);
        assert(c.rect().width() == 34u);  // last gets the remainder
    }

    /* --- 3. per-corner: TL|TR rounded, bottom square --- */
    {
        constexpr uint32_t kW                = 40;
        uint8_t            buf[kW * kW * 4u] = {};
        Surface            s{buf, kW, kW, kW * 4u, PixelFormat::kXrgb8888};
        fill_rounded_rect(s, 10, 10, 20, 20, 0x00FF0000u, 6, nullptr, kCornerTL | kCornerTR);
        const uint32_t* px = reinterpret_cast<const uint32_t*>(buf);
        assert(px[10 * kW + 10] == 0u);           // top-left: arc bites -> bg
        assert(px[29 * kW + 10] == 0x00FF0000u);  // bottom-left: square -> red
        assert(px[20 * kW + 20] == 0x00FF0000u);  // centre -> red
    }

    std::printf("test_flex: OK\n");
    return 0;
}
