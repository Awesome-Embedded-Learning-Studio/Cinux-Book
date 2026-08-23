/**
 * @file test/test_dirty.cpp
 * @brief P5-c -- per-widget dirty + Desktop::render idle skip
 *
 * Asserts the idle discipline:
 *   - first render after setup is dirty (add_window invalidated the tree)
 *   - a second render with no changes is idle (0 dirty rects -> 0 flush)
 *   - a pointer event (cursor move) re-invalidates -> dirty again
 *   - another idle render -> 0 rects
 *
 * Standalone ctest. Pure logic over Desktop::render's Region out-param.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"          // PointerPayload
#include "font.hpp"                   // PsfFont
#include "region.hpp"                 // Region
#include "swraster.hpp"               // Surface
#include "widget.hpp"                 // Desktop
#include "widget/window.hpp"          // Window
#include "widget/window_manager.hpp"  // WindowManager

using namespace cinux::gui;

int main() {
    PsfFont font;
    font.init();

    constexpr uint32_t kW  = 160;
    constexpr uint32_t kH  = 120;
    uint8_t*           buf = new uint8_t[kW * kH * 4u]();
    Surface            staging{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};

    WindowManager wm;
    wm.set_rect(0, 0, kW, kH);
    Window w;
    w.set_rect(40, 30, 80, 60);
    wm.add_window(&w);  // invalidates wm

    Desktop d;
    d.set_root(&wm);

    Region dirty;

    /* frame 1: dirty (add_window invalidated the tree) */
    d.render(staging, font, &dirty);
    assert(dirty.count() > 0u);

    /* frame 2: idle (nothing changed since clear_dirty) */
    d.render(staging, font, &dirty);
    assert(dirty.count() == 0u);

    /* a pointer event re-invalidates (cursor moved) */
    PointerPayload p{};
    p.kind = kPointerKindMove;
    p.x    = 100;
    p.y    = 60;
    wm.process_pointer(p);
    d.render(staging, font, &dirty);
    assert(dirty.count() > 0u);

    /* idle again */
    d.render(staging, font, &dirty);
    assert(dirty.count() == 0u);

    delete[] buf;
    std::printf("test_dirty: OK\n");
    return 0;
}
