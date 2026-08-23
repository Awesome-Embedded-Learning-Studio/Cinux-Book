/**
 * @file test/test_resize.cpp
 * @brief P6-b Window -- resize grip drag + maximize/restore + minimize
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"  // PointerPayload
#include "region.hpp"         // Rect
#include "widget.hpp"         // Widget
#include "widget/window.hpp"  // Window

using namespace cinux::gui;

static PointerPayload ptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    Window w;
    w.set_rect(0, 0, 100, 80);  // resize grip at (92..100, 72..80)

    /* --- 1. resize grip drag grows the window --- */
    w.on_pointer(ptr(kPointerKindDown, 95, 75));   // press grip
    w.on_pointer(ptr(kPointerKindMove, 105, 85));  // drag +10,+10
    assert(w.rect().width() == 110);
    assert(w.rect().height() == 90);
    w.on_pointer(ptr(kPointerKindUp, 105, 85));
    assert(!w.maximized());

    /* --- 2. maximize fills full, restore returns to prior --- */
    w.set_maximized(true, Rect{0, 0, 300, 200});
    assert(w.maximized());
    assert(w.rect().width() == 300 && w.rect().height() == 200);
    w.set_maximized(false, Rect{0, 0, 300, 200});  // full ignored on restore
    assert(!w.maximized());
    assert(w.rect().width() == 110 && w.rect().height() == 90);

    /* --- 3. minimize hides (visible false) --- */
    assert(w.visible());
    w.set_minimized(true);
    assert(w.minimized());
    assert(!w.visible());
    w.set_minimized(false);
    assert(w.visible());

    std::printf("test_resize: OK\n");
    return 0;
}
