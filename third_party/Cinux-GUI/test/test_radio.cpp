/**
 * @file test/test_radio.cpp
 * @brief P7-a Radio + RadioGroup -- exclusive selection + paint
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"  // PointerPayload
#include "paint_list.hpp"     // PaintList
#include "theme.hpp"          // material_light
#include "widget.hpp"         // Widget
#include "widget/radio.hpp"   // Radio, RadioGroup

using namespace cinux::gui;

static PointerPayload ptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    Radio      r1;
    Radio      r2;
    RadioGroup g;
    g.add(&r1);
    g.add(&r2);
    r1.set_group(&g);
    r2.set_group(&g);
    Theme th = material_light();
    r1.set_theme(&th);
    r2.set_theme(&th);

    assert(!r1.checked() && !r2.checked());

    /* select r1 -> only r1 checked, group tracks it */
    r1.on_pointer(ptr(kPointerKindDown, 5, 5));
    assert(r1.checked());
    assert(!r2.checked());
    assert(g.selected() == &r1);

    /* select r2 -> r1 cleared (exclusive) */
    r2.on_pointer(ptr(kPointerKindDown, 5, 5));
    assert(!r1.checked());
    assert(r2.checked());
    assert(g.selected() == &r2);

    /* paint yields at least outer + hollow (+ dot when checked) */
    PaintList list;
    r1.flatten(list);
    assert(list.count() >= 2);

    std::printf("test_radio: OK\n");
    return 0;
}
