/**
 * @file test/test_window_manager.cpp
 * @brief P4-b WindowManager -- Z-order, raise, click-to-raise, close, cursor
 *
 * Drives a WindowManager with two overlapping windows via process_pointer and
 * asserts:
 *   - add_window stacks newest on top (topmost / Z-order)
 *   - raise() brings a window to the top
 *   - click-to-raise: pressing an occluded window's exposed area raises it
 *   - close (x press+release) removes the window from the Z-order
 *   - process_pointer tracks the cursor position
 *   - flatten yields a background fill + window paint + cursor fill
 *
 * Standalone ctest. Pure logic (no host, no framebuffer).
 *
 * Compile condition: CINUX_GUI (via the core lib).
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"          // PointerPayload, kPointerKind*
#include "paint_list.hpp"             // PaintList, CmdKind
#include "widget.hpp"                 // Widget
#include "widget/window.hpp"          // Window
#include "widget/window_manager.hpp"  // WindowManager

using namespace cinux::gui;

static PointerPayload mkptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    /* --- 1. add_window stacks newest on top --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        Window w1;
        w1.set_rect(0, 0, 100, 80);
        Window w2;
        w2.set_rect(50, 0, 100, 80);  // overlaps w1's right half
        wm.add_window(&w1);
        wm.add_window(&w2);

        assert(wm.window_count() == 2);
        assert(wm.topmost() == &w2);     // newest on top
        assert(wm.window_at(0) == &w1);  // bottom
        assert(wm.window_at(1) == &w2);  // top
    }

    /* --- 2. raise() brings a window to the top --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        Window w1;
        w1.set_rect(0, 0, 100, 80);
        Window w2;
        w2.set_rect(50, 0, 100, 80);
        wm.add_window(&w1);
        wm.add_window(&w2);

        wm.raise(&w1);
        assert(wm.topmost() == &w1);
    }

    /* --- 3. click-to-raise: press an occluded window's exposed area --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        Window w1;
        w1.set_rect(0, 0, 100, 80);  // exposed at x < 50
        Window w2;
        w2.set_rect(50, 0, 100, 80);  // occludes w1's right half
        wm.add_window(&w1);
        wm.add_window(&w2);
        assert(wm.topmost() == &w2);

        wm.process_pointer(mkptr(kPointerKindDown, 10, 40));  // w1's exposed left
        assert(wm.topmost() == &w1);                          // raised
    }

    /* --- 4. close (x press+release) removes the window --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        Window w;
        w.set_rect(0, 0, 100, 80);  // close button at x[80,100) y[0,20)
        wm.add_window(&w);
        assert(wm.window_count() == 1);

        wm.process_pointer(mkptr(kPointerKindDown, 90, 10));  // press close
        wm.process_pointer(mkptr(kPointerKindUp, 90, 10));    // release close
        assert(wm.window_count() == 0);
        assert(wm.topmost() == nullptr);
    }

    /* --- 5. process_pointer tracks the cursor --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        wm.process_pointer(mkptr(kPointerKindMove, 123, 45));
        assert(wm.cursor_x() == 123 && wm.cursor_y() == 45);
    }

    /* --- 6. flatten yields bg fill + window paint + cursor fill --- */
    {
        WindowManager wm;
        wm.set_rect(0, 0, 300, 200);
        Window w;
        w.set_title("App");
        w.set_rect(10, 10, 100, 80);
        wm.add_window(&w);
        wm.process_pointer(mkptr(kPointerKindMove, 50, 50));  // place cursor

        PaintList list;
        wm.flatten(list);

        bool has_fill = false;
        for (uint32_t i = 0; i < list.count(); ++i) {
            if (list.at(i).kind == CmdKind::kFillRect) {
                has_fill = true;
            }
        }
        assert(has_fill);          // bg + cursor are both kFillRect
        assert(list.count() > 2);  // clip push/pop + bg + window cmds + cursor
    }

    std::printf("test_window_manager: OK\n");
    return 0;
}
