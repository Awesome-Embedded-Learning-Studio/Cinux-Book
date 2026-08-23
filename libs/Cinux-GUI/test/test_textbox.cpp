/**
 * @file test/test_textbox.cpp
 * @brief P6-a TextBox -- keyboard insert/delete via Desktop focus routing
 *
 * Clicks the box (Desktop sets focus), then dispatches keys and asserts text +
 * cursor. Paint check: flatten yields a fill + the text + a cursor fill.
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "event_payload.hpp"   // KeycodePayload, PointerPayload
#include "paint_list.hpp"      // PaintList, CmdKind
#include "theme.hpp"           // material_light
#include "widget.hpp"          // Desktop
#include "widget/textbox.hpp"  // TextBox

using namespace cinux::gui;

static KeycodePayload key(char ch) {
    KeycodePayload k{};
    k.ascii = ch;
    return k;
}
static PointerPayload ptr(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

int main() {
    TextBox tb;
    tb.set_rect(0, 0, 200, 24);
    Theme th = material_light();
    tb.set_theme(&th);

    Desktop d;
    d.set_root(&tb);
    d.dispatch_pointer(ptr(kPointerKindDown, 10, 10));  // click -> focus textbox
    d.dispatch_key(key('A'));
    d.dispatch_key(key('B'));
    d.dispatch_key(key('C'));

    assert(tb.length() == 3);
    assert(tb.cursor() == 3);
    assert(tb.text()[0] == 'A' && tb.text()[1] == 'B' && tb.text()[2] == 'C');

    d.dispatch_key(key('\b'));  // erase 'C'
    assert(tb.length() == 2);
    assert(tb.cursor() == 2);
    assert(tb.text()[0] == 'A' && tb.text()[1] == 'B');

    /* Paint: text cmd + (bg + cursor) fills. */
    PaintList list;
    tb.flatten(list);
    bool has_text = false;
    bool has_fill = false;
    for (uint32_t i = 0; i < list.count(); ++i) {
        const PaintCmd& c = list.at(i);
        if (c.kind == CmdKind::kText) {
            has_text = true;
        }
        if (c.kind == CmdKind::kFillRect) {
            has_fill = true;
        }
    }
    assert(has_text && has_fill);

    std::printf("test_textbox: OK\n");
    return 0;
}
