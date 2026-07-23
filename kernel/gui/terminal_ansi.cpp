/**
 * @file kernel/gui/terminal_ansi.cpp
 * @brief Terminal ANSI escape sequence handling
 *
 * Split from terminal.cpp to keep that TU under the 500-line CI line-limit
 * (see c9cfd02 for the same split pattern).  Holds the CSI parser
 * (is_escape / handle_ansi) that Terminal::write() dispatches into; the rest
 * of the Terminal implementation stays in terminal.cpp.
 */

#include "terminal.hpp"

#include <cstdint>

namespace cinux::gui {

// ============================================================
// CSI sequence detection
// ============================================================

bool Terminal::is_escape(char ch) {
    return ch == '\033';
}

// ============================================================
// CSI sequence handling
// ============================================================

void Terminal::handle_ansi(const char* str, uint64_t len, uint64_t& pos) {
    // Expect ESC followed by '[' (CSI sequence)
    if (pos + 1 >= len || str[pos + 1] != '[') {
        // Not a CSI sequence, skip the ESC character
        pos++;
        return;
    }

    // Skip ESC and '['
    pos += 2;

    // Collect parameter bytes (digits and semicolons)
    // and the final byte (letter)
    uint32_t param = 0;

    while (pos < len) {
        char ch = str[pos];

        if (ch >= '0' && ch <= '9') {
            // Build numeric parameter
            param = param * 10 + static_cast<uint32_t>(ch - '0');
            pos++;
        } else if (ch == ';') {
            // Skip separator
            pos++;
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            // Final byte -- dispatch
            pos++;

            switch (ch) {
            case 'J': {
                // ED -- Erase in Display.  busybox `clear` emits ESC[H ESC[J
                // (home + erase cursor-to-end), which is the whole screen after
                // home; only handling ESC[2J left ESC[J a no-op, so `clear`
                // moved the cursor but erased nothing.
                //   param 0 (ESC[J / ESC[0J): cursor -> end of screen (default)
                //   param 1 (ESC[1J): start of screen -> cursor
                //   param 2 (ESC[2J): entire screen
                if (param == 2) {
                    clear();
                } else if (param == 1) {
                    for (uint32_t r = 0; r < cursor_y_; r++) {
                        for (uint32_t c = 0; c < COLS; c++) {
                            screen_[r][c] = TerminalCell{};
                        }
                    }
                    for (uint32_t c = 0; c < cursor_x_; c++) {
                        screen_[cursor_y_][c] = TerminalCell{};
                    }
                } else {  // param 0 (ESC[J default): cursor -> end of screen
                    for (uint32_t c = cursor_x_; c < COLS; c++) {
                        screen_[cursor_y_][c] = TerminalCell{};
                    }
                    for (uint32_t r = cursor_y_ + 1; r < ROWS; r++) {
                        for (uint32_t c = 0; c < COLS; c++) {
                            screen_[r][c] = TerminalCell{};
                        }
                    }
                }
                return;
            }

            case 'H':
                // ESC[H: cursor home (row 1, col 1 in ANSI -> row 0, col 0 here)
                cursor_x_ = 0;
                cursor_y_ = 0;
                return;

            case 'K':
                // ESC[K: clear from cursor to end of line
                for (uint32_t c = cursor_x_; c < COLS; c++) {
                    screen_[cursor_y_][c] = TerminalCell{};
                }
                return;

            case 'm':
                // ESC[m: SGR (Select Graphic Rendition)
                // For now, just ignore (no colour support in basic terminal)
                return;

            default:
                // Unknown sequence, stop parsing
                return;
            }
        } else {
            // Unexpected character, stop parsing
            return;
        }
    }
}

}  // namespace cinux::gui
