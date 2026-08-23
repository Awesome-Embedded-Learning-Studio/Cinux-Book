/**
 * @file test/test_window.cpp
 * @brief P4-a Window widget -- title-bar geometry, hit-test, drag, close
 *
 * Drives a Window through a Desktop (press capture) and asserts:
 *   - layout() places content below the title bar
 *   - hit_test: title bar + close button target the Window; content targets
 *     the content widget; outside -> nullptr
 *   - title-bar press-drag moves the window rect by the drag delta (content
 *     follows); the drag survives because Desktop press capture routes moves
 *     to the Window
 *   - close button press+release fires on_close; releasing outside the close
 *     button does not
 *   - flatten() yields a PaintList with a rounded body + a title band + text
 *
 * Standalone ctest. Pure logic (no host, no framebuffer).
 *
 * Compile condition: CINUX_GUI (via the core lib).
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"  // PointerPayload, kPointerKind*
#include "paint_list.hpp"     // PaintList, PaintCmd, CmdKind
#include "theme.hpp"          // material_light
#include "widget.hpp"         // Desktop, Widget
#include "widget/label.hpp"   // Label
#include "widget/window.hpp"  // Window

using namespace cinux::gui;

static PointerPayload mkptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

static void close_cb(void* ctx, Window* /*w*/) { ++*static_cast<int*>(ctx); }

int main() {
    const int32_t tb = static_cast<int32_t>(Window::kTitleBarHeight);

    /* --- 1. layout: content sits below the title bar --- */
    {
        Window w;
        w.set_rect(10, 20, 100, 80);
        Label content;
        w.set_content(&content);
        w.layout();

        const Rect cr = w.content_rect();
        assert(cr.x0 == 10 && cr.y0 == 20 + tb);
        assert(cr.x1 == 110 && cr.y1 == 100);
        assert(content.rect().x0 == cr.x0 && content.rect().y0 == cr.y0 &&
               content.rect().x1 == cr.x1 && content.rect().y1 == cr.y1);
    }

    /* --- 2. hit_test: title bar / close / content / outside --- */
    {
        Window w;
        w.set_rect(10, 20, 100, 80);
        Label content;
        w.set_content(&content);
        w.layout();

        assert(w.hit_test(15, 25) == &w);         // title bar
        assert(w.hit_test(100, 25) == &w);        // close button (x 90..110)
        assert(w.hit_test(50, 60) == &content);   // content area
        assert(w.hit_test(200, 200) == nullptr);  // outside
    }

    /* --- 3. drag via Desktop press capture --- */
    {
        Window w;
        w.set_rect(10, 20, 100, 80);
        Label content;
        w.set_content(&content);
        w.layout();

        Desktop d;
        d.set_root(&w);
        d.dispatch_pointer(mkptr(kPointerKindDown, 15, 25));  // press title bar
        d.dispatch_pointer(mkptr(kPointerKindMove, 45, 45));  // drag, delta (30,20)

        assert(w.rect().x0 == 40 && w.rect().y0 == 40);  // origin 10,20 + (30,20)
        assert(content.rect().x0 == 40 && content.rect().y0 == 40 + tb);
    }

    /* --- 4. close: press+release inside fires on_close --- */
    {
        Window w;
        w.set_rect(10, 20, 100, 80);
        Label content;
        w.set_content(&content);
        w.layout();
        int close_count = 0;
        w.set_on_close(close_cb, &close_count);

        Desktop d;
        d.set_root(&w);
        d.dispatch_pointer(mkptr(kPointerKindDown, 100, 25));  // press close
        d.dispatch_pointer(mkptr(kPointerKindUp, 100, 25));    // release close
        assert(close_count == 1);
    }

    /* --- 5. close: release outside the close button -> no fire --- */
    {
        Window w;
        w.set_rect(10, 20, 100, 80);
        Label content;
        w.set_content(&content);
        w.layout();
        int close_count = 0;
        w.set_on_close(close_cb, &close_count);

        Desktop d;
        d.set_root(&w);
        d.dispatch_pointer(mkptr(kPointerKindDown, 100, 25));  // press close
        d.dispatch_pointer(mkptr(kPointerKindUp, 50, 60));     // release in content
        assert(close_count == 0);
    }

    /* --- 6. flatten yields rounded body + title band + text --- */
    {
        Window w;
        Theme  th = material_light();
        w.set_theme(&th);
        w.set_title("My App");
        w.set_rect(10, 20, 100, 80);
        Label content;
        content.set_text("body");
        w.set_content(&content);
        w.layout();

        PaintList list;
        w.flatten(list);

        bool has_round = false;
        bool has_fill  = false;
        bool has_text  = false;
        for (uint32_t i = 0; i < list.count(); ++i) {
            const PaintCmd& c = list.at(i);
            if (c.kind == CmdKind::kFillRoundRect) {
                has_round = true;
            }
            if (c.kind == CmdKind::kFillRect) {
                has_fill = true;
            }
            if (c.kind == CmdKind::kText) {
                has_text = true;
            }
        }
        assert(has_round && has_fill && has_text);
    }

    std::printf("test_window: OK\n");
    return 0;
}
