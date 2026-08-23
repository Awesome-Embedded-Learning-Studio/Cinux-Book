/**
 * @file test/test_terminal_ansi.cpp
 * @brief P5-e -- TerminalWidget ANSI SGR colour, cursor, clear
 *
 * Feeds escape sequences through write() and asserts:
 *   - SGR sets the per-cell fg (31=red, 32=green), 0/39 resets to white
 *   - bright colours (91) map to index 9
 *   - cursor position [row;colH moves the write cursor (overwrite)
 *   - [2J clears the grid
 *
 * Standalone ctest.
 */
#include <stdint.h>

#include <cassert>
#include <cstdio>

#include "widget.hpp"           // Widget
#include "widget/terminal.hpp"  // TerminalWidget

using namespace cinux::gui;

int main() {
    /* --- 1. SGR fg colours stamp onto cells --- */
    {
        TerminalWidget t;
        t.write("\x1b[31mR\x1b[32mG");
        assert(t.cell_at(0, 0) == 'R');
        assert(t.fg_at(0, 0) == 1u);  // red
        assert(t.cell_at(1, 0) == 'G');
        assert(t.fg_at(1, 0) == 2u);  // green
    }

    /* --- 2. SGR 0 / 39 reset to default white (7) --- */
    {
        TerminalWidget t;
        t.write("\x1b[31mR\x1b[0mW");
        assert(t.fg_at(0, 0) == 1u);  // R still red
        assert(t.fg_at(1, 0) == 7u);  // W reset to white
    }

    /* --- 3. bright colour (91) -> index 9 --- */
    {
        TerminalWidget t;
        t.write("\x1b[91mB");
        assert(t.fg_at(0, 0) == 9u);
    }

    /* --- 4. cursor position [row;colH moves the cursor (overwrite) --- */
    {
        TerminalWidget t;
        t.write("AB\x1b[1;1HX");  // home then X -> overwrites A
        assert(t.cell_at(0, 0) == 'X');
        assert(t.cell_at(1, 0) == 'B');
    }

    /* --- 5. [2J clears the grid --- */
    {
        TerminalWidget t;
        t.write("AB");
        assert(t.cell_at(0, 0) == 'A');
        t.write("\x1b[2J");
        assert(t.cell_at(0, 0) == 0);
        assert(t.cell_at(1, 0) == 0);
    }

    std::printf("test_terminal_ansi: OK\n");
    return 0;
}
