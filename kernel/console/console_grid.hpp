/**
 * @file    console_grid.hpp
 * @brief   Caret math for a text grid: where the next character lands,
 *          and when the grid has to scroll.
 *
 * Pure constexpr state transitions over a row/column pair. The device
 * side owns painting; this header owns the grammar — newline, carriage
 * return, backspace, and the auto-wrap that fires one step before the
 * cursor would leave the grid. The returned caret always satisfies
 * column < columns and row < total_rows.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::console {

/// Where the next character lands: text row and column.
struct Caret {
    uint32_t row;
    uint32_t column;
};

/// One caret transition: the next caret plus whether the grid scrolled.
struct CaretStep {
    Caret caret;
    bool  scrolled;
};

/// Newline in the char grammar.
inline constexpr char kCharNewline = '\n';

/// Carriage return in the char grammar.
inline constexpr char kCharReturn = '\r';

/// Backspace in the char grammar.
inline constexpr char kCharBackspace = '\b';

/**
 * @brief         Drop to column zero and advance one row, scrolling in
 *                place at the bottom row instead of leaving the grid.
 *
 * @param[in]     current     Caret before the step.
 * @param[in]     total_rows  Text rows in the grid.
 * @return        The next caret and whether the grid scrolled.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr CaretStep AdvanceLine(Caret current, uint32_t total_rows) {
    if (current.row + 1U >= total_rows) {
        return CaretStep{.caret = {.row = current.row, .column = 0}, .scrolled = true};
    }
    return CaretStep{.caret = {.row = current.row + 1U, .column = 0}, .scrolled = false};
}

/**
 * @brief         Step the caret once for one character: printable glyphs
 *                consume a cell and auto-wrap at the right edge, control
 *                characters move the caret their own way, and stepping
 *                past the bottom row scrolls instead of leaving the grid.
 *
 * @param[in]     current     Caret before the character; must satisfy
 *                            column < columns and row < total_rows.
 * @param[in]     columns     Text columns in the grid.
 * @param[in]     total_rows  Text rows in the grid.
 * @param[in]     glyph_char  The character being placed.
 * @return        The next caret and whether the grid scrolled.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
constexpr CaretStep AdvanceCaret(Caret current, uint32_t columns, uint32_t total_rows,
                                 char glyph_char) {
    if (glyph_char == kCharNewline) {
        return AdvanceLine(current, total_rows);
    }
    if (glyph_char == kCharReturn) {
        return CaretStep{.caret = {.row = current.row, .column = 0}, .scrolled = false};
    }
    if (glyph_char == kCharBackspace) {
        uint32_t const kBacked = (current.column > 0) ? current.column - 1 : 0;
        return CaretStep{.caret = {.row = current.row, .column = kBacked}, .scrolled = false};
    }
    if (current.column + 1U < columns) {
        return CaretStep{.caret    = {.row = current.row, .column = current.column + 1U},
                         .scrolled = false};
    }
    return AdvanceLine(current, total_rows);
}

}  // namespace cinux::console
