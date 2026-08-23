/**
 * @file test/test_dropdown.cpp
 * @brief P7-a Dropdown -- open / select-row / close + paint
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "event_payload.hpp"    // PointerPayload
#include "paint_list.hpp"       // PaintList
#include "theme.hpp"            // material_light
#include "widget.hpp"           // Widget
#include "widget/dropdown.hpp"  // Dropdown

using namespace cinux::gui;

static PointerPayload ptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    Dropdown d;
    d.set_option(0, "Apple");
    d.set_option(1, "Banana");
    d.set_option(2, "Cherry");
    d.set_option_count(3);
    d.set_rect(0, 0, 100, 3u * Dropdown::kRowH);  // 3 rows tall
    Theme th = material_light();
    d.set_theme(&th);

    assert(!d.expanded());
    assert(d.selected() == 0u);

    /* first click opens the list */
    d.on_pointer(ptr(kPointerKindDown, 10, 5));
    assert(d.expanded());

    /* click row 2 (Cherry) -> select + close */
    d.on_pointer(ptr(kPointerKindDown, 10, static_cast<int32_t>(2u * Dropdown::kRowH) + 5));
    assert(!d.expanded());
    assert(d.selected() == 2u);
    assert(std::strcmp(d.selected_text(), "Cherry") == 0);

    /* paint yields something (bg + at least the selected row) */
    PaintList list;
    d.flatten(list);
    assert(list.count() >= 2);

    std::printf("test_dropdown: OK\n");
    return 0;
}
