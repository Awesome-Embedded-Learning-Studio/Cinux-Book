/**
 * @file test/test_checkbox.cpp
 * @brief P6-a CheckBox -- click toggles checked + paint
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"    // PointerPayload
#include "paint_list.hpp"       // PaintList
#include "theme.hpp"            // material_light
#include "widget.hpp"           // Widget
#include "widget/checkbox.hpp"  // CheckBox

using namespace cinux::gui;

static PointerPayload ptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    CheckBox cb;
    cb.set_rect(0, 0, 24, 24);
    Theme th = material_light();
    cb.set_theme(&th);

    assert(!cb.checked());
    cb.on_pointer(ptr(kPointerKindDown, 8, 8));  // press -> on
    assert(cb.checked());
    cb.on_pointer(ptr(kPointerKindDown, 8, 8));  // press -> off
    assert(!cb.checked());

    /* Paint: outline + hollow + (if checked) check fill -> several fills. */
    PaintList list;
    cb.flatten(list);
    assert(list.count() >= 2);

    std::printf("test_checkbox: OK\n");
    return 0;
}
