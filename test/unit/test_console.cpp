#include <cstdint>

#include "../framework/framework.hpp"
#include "kernel/console/console_grid.hpp"
#include "test_assert.hpp"

using cinux::console::AdvanceCaret;
using cinux::console::Caret;

namespace {

constexpr uint32_t kColumns = 10;
constexpr uint32_t kRows    = 24;

constexpr Caret kMidField{.row = 2, .column = 5};
constexpr Caret kLastRow{.row = 23, .column = 5};
constexpr Caret kLineEnd{.row = 2, .column = 9};
constexpr Caret kCorner{.row = 23, .column = 9};

}  // namespace

TEST("console grid: newline mid-field drops a row and homes") {
    auto const kStep = AdvanceCaret(kMidField, kColumns, kRows, '\n');
    ASSERT_EQ(kStep.caret.row, 3U);
    ASSERT_EQ(kStep.caret.column, 0U);
    ASSERT_EQ(kStep.scrolled, false);
}

TEST("console grid: newline on the last row scrolls in place") {
    auto const kStep = AdvanceCaret(kLastRow, kColumns, kRows, '\n');
    ASSERT_EQ(kStep.caret.row, 23U);
    ASSERT_EQ(kStep.caret.column, 0U);
    ASSERT_TRUE(kStep.scrolled);
}

TEST("console grid: carriage return homes the column only") {
    auto const kStep = AdvanceCaret(kMidField, kColumns, kRows, '\r');
    ASSERT_EQ(kStep.caret.row, 2U);
    ASSERT_EQ(kStep.caret.column, 0U);
    ASSERT_EQ(kStep.scrolled, false);
}

TEST("console grid: backspace steps back one column") {
    auto const kStep = AdvanceCaret(kMidField, kColumns, kRows, '\b');
    ASSERT_EQ(kStep.caret.row, 2U);
    ASSERT_EQ(kStep.caret.column, 4U);
}

TEST("console grid: backspace at column zero holds the wall") {
    Caret const kHomeCol{.row = 2, .column = 0};
    auto const  kStep = AdvanceCaret(kHomeCol, kColumns, kRows, '\b');
    ASSERT_TRUE(kStep.caret.column == 0);
}

TEST("console grid: a printable glyph consumes one cell") {
    auto const kStep = AdvanceCaret(kMidField, kColumns, kRows, 'A');
    ASSERT_EQ(kStep.caret.row, 2U);
    ASSERT_EQ(kStep.caret.column, 6U);
    ASSERT_EQ(kStep.scrolled, false);
}

TEST("console grid: painting the last column wraps to the next line") {
    auto const kStep = AdvanceCaret(kLineEnd, kColumns, kRows, 'A');
    ASSERT_EQ(kStep.caret.row, 3U);
    ASSERT_EQ(kStep.caret.column, 0U);
    ASSERT_EQ(kStep.scrolled, false);
}

TEST("console grid: wrapping at the bottom scrolls") {
    auto const kStep = AdvanceCaret(kCorner, kColumns, kRows, 'A');
    ASSERT_EQ(kStep.caret.row, 23U);
    ASSERT_EQ(kStep.caret.column, 0U);
    ASSERT_TRUE(kStep.scrolled);
}

TEST("console grid: every step keeps the caret inside the grid") {
    char const kAll[] = {'a', '\n', '\r', '\b', 'Z'};
    for (uint32_t row = 0; row < kRows; ++row) {
        for (uint32_t column = 0; column < kColumns; ++column) {
            for (char const kProbe : kAll) {
                auto const kStep =
                    AdvanceCaret(Caret{.row = row, .column = column}, kColumns, kRows, kProbe);
                ASSERT_TRUE(kStep.caret.column < kColumns);
                ASSERT_TRUE(kStep.caret.row < kRows);
            }
        }
    }
}

int main() {
    return cinux::test::RunAll();
}
