/**
 * @file test/test_terminal_bg256.cpp
 * @brief P6-c TerminalWidget -- ANSI bg (40-47) + 256 colour (38;5;N) + cursor block
 *
 * Standalone ctest. Pure logic.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "paint_list.hpp"       // PaintList, CmdKind
#include "widget.hpp"           // Widget
#include "widget/terminal.hpp"  // TerminalWidget

using namespace cinux::gui;

int main() {
    /* --- 1. bg SGR (40-47) sets the per-cell bg, fg stays default --- */
    {
        TerminalWidget t;
        t.set_cols_rows(10, 3);
        t.set_rect(0, 0, 80, 48);
        t.write("\x1b[41mR\x1b[42mG");  // red bg + 'R', green bg + 'G'
        assert(t.bg_at(0, 0) == 1u);    // red
        assert(t.bg_at(1, 0) == 2u);    // green
        assert(t.fg_at(0, 0) == 7u);    // default white fg
    }

    /* --- 2. 256-colour fg (38;5;N) --- */
    {
        TerminalWidget t;
        t.set_cols_rows(10, 3);
        t.set_rect(0, 0, 80, 48);
        t.write("\x1b[38;5;200mA");
        assert(t.fg_at(0, 0) == 200u);
    }

    /* --- 3. 256-colour bg (48;5;N) + reset clears it --- */
    {
        TerminalWidget t;
        t.set_cols_rows(10, 3);
        t.set_rect(0, 0, 80, 48);
        t.write("\x1b[48;5;100B");
        assert(t.bg_at(0, 0) == 100u);
        t.write("\x1b[0mC");  // reset -> default bg
        assert(t.bg_at(1, 0) == 0u);
    }

    /* --- 4. cursor block in flatten (bg fill + glyph + cursor fill) --- */
    {
        TerminalWidget t;
        t.set_cols_rows(10, 3);
        t.set_rect(0, 0, 80, 48);
        t.write("X");  // cursor now at (1,0)

        PaintList list;
        t.flatten(list);
        uint32_t fills  = 0;
        uint32_t glyphs = 0;
        for (uint32_t i = 0; i < list.count(); ++i) {
            const PaintCmd& c = list.at(i);
            if (c.kind == CmdKind::kFillRect) {
                ++fills;
            }
            if (c.kind == CmdKind::kTextGlyph) {
                ++glyphs;
            }
        }
        assert(glyphs >= 1);  // the 'X'
        assert(fills >= 2);   // bg fill + cursor block
    }

    std::printf("test_terminal_bg256: OK\n");
    return 0;
}
