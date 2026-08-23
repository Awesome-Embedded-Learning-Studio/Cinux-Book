/**
 * @file core/widget/terminal.cpp
 * @brief cinux::gui TerminalWidget -- write/scroll/paint + ANSI SGR/cursor/clear (P4-c, P5-e)
 *
 * Compile condition: CINUX_GUI.
 */

#include "terminal.hpp"

#include <stdint.h>

#include "../colors.hpp"

namespace cinux::gui {

namespace {
/* P6-c: 256-colour palette lookup (XRGB8888). 0-15 = the 16 in colors.hpp;
 * 16-231 a 6x6x6 colour cube (each channel 0 or 55+40*v); 232-255 grayscale. */
uint32_t palette_color(uint8_t idx) {
    if (idx < 16u) {
        return colors::kAnsiPalette[idx];
    }
    if (idx < 232u) {
        const uint32_t c    = idx - 16u;
        const auto     chan = [](uint32_t v) { return v == 0u ? 0u : (55u + 40u * v); };
        const uint32_t r    = chan(c / 36u);
        const uint32_t g    = chan((c / 6u) % 6u);
        const uint32_t b    = chan(c % 6u);
        return (r << 16) | (g << 8) | b;
    }
    const uint32_t v = 8u + (static_cast<uint32_t>(idx) - 232u) * 10u;
    return (v << 16) | (v << 8) | v;
}
}  // namespace

void TerminalWidget::set_cols_rows(uint32_t cols, uint32_t rows) {
    cols_ = (cols > kMaxCols) ? kMaxCols : cols;
    rows_ = (rows > kMaxRows) ? kMaxRows : rows;
    clear();
}

void TerminalWidget::clear() {
    for (uint32_t i = 0; i < kMaxCols * kMaxRows; ++i) {
        cells_[i] = 0;
    }
    cur_col_    = 0;
    cur_row_    = 0;
    cur_fg_     = 7u;  // P5-e: default white
    ansi_state_ = AnsiState::kNormal;
    csi_len_    = 0u;
    dirty_all_  = true;  // P5-f: whole grid dirty
    invalidate();        // P5-c: grid cleared
}

char TerminalWidget::cell_at(uint32_t col, uint32_t row) const {
    if (col >= kMaxCols || row >= kMaxRows) {
        return 0;
    }
    return cells_[row * kMaxCols + col];
}

uint8_t TerminalWidget::fg_at(uint32_t col, uint32_t row) const {
    if (col >= kMaxCols || row >= kMaxRows) {
        return 0u;
    }
    return fg_colors_[row * kMaxCols + col];  // P5-e
}

uint8_t TerminalWidget::bg_at(uint32_t col, uint32_t row) const {  // P6-c
    if (col >= kMaxCols || row >= kMaxRows) {
        return 0u;
    }
    return bg_colors_[row * kMaxCols + col];
}

void TerminalWidget::scroll_up_() {
    for (uint32_t r = 1; r < rows_; ++r) {
        for (uint32_t c = 0; c < kMaxCols; ++c) {
            const uint32_t src = r * kMaxCols + c;
            const uint32_t dst = (r - 1) * kMaxCols + c;
            cells_[dst]        = cells_[src];
            fg_colors_[dst]    = fg_colors_[src];  // P5-e: colour scrolls with the char
            bg_colors_[dst]    = bg_colors_[src];  // P6-c: bg scrolls too
        }
    }
    for (uint32_t c = 0; c < kMaxCols; ++c) {
        cells_[(rows_ - 1) * kMaxCols + c]     = 0;
        fg_colors_[(rows_ - 1) * kMaxCols + c] = 0;
        bg_colors_[(rows_ - 1) * kMaxCols + c] = 0;
    }
    dirty_all_  = true;  // P5-f: a scroll repaints the whole grid
    dirty_self_ = true;
}

void TerminalWidget::newline_() {
    cur_col_ = 0;
    ++cur_row_;
    if (cur_row_ >= rows_) {
        scroll_up_();
        cur_row_ = rows_ - 1;
    }
}

void TerminalWidget::apply_sgr_() {
    /* P6-c: parse csi_param_ into a codes[] array so multi-code sequences
     * (38;5;N / 48;5;N) can peek ahead. Empty param = 0 = reset. */
    uint32_t codes[16];
    uint32_t ncode = 0;
    uint32_t cur   = 0;
    bool     has   = false;
    for (uint8_t i = 0; i < csi_len_ && ncode < 16u; ++i) {
        const char b = csi_param_[i];
        if (b >= '0' && b <= '9') {
            cur = cur * 10u + static_cast<uint32_t>(b - '0');
            has = true;
        } else if (b == ';') {
            codes[ncode++] = has ? cur : 0u;
            cur            = 0;
            has            = false;
        }
    }
    codes[ncode++] = has ? cur : 0u;  // last (or lone 0 when empty)

    for (uint32_t i = 0u; i < ncode; ++i) {
        const uint32_t c = codes[i];
        if (c == 0u || c == 39u) {
            cur_fg_ = 7u;
            cur_bg_ = 0u;  // reset / default fg + bg
        } else if (c == 49u) {
            cur_bg_ = 0u;  // default bg
        } else if (c >= 30u && c <= 37u) {
            cur_fg_ = static_cast<uint8_t>(c - 30u);
        } else if (c >= 90u && c <= 97u) {
            cur_fg_ = static_cast<uint8_t>(c - 90u + 8u);  // bright fg
        } else if (c >= 40u && c <= 47u) {
            cur_bg_ = static_cast<uint8_t>(c - 40u);  // P6-c: bg
        } else if (c >= 100u && c <= 107u) {
            cur_bg_ = static_cast<uint8_t>(c - 100u + 8u);  // bright bg
        } else if (c == 38u && i + 2u < ncode && codes[i + 1u] == 5u) {
            cur_fg_ = static_cast<uint8_t>(codes[i + 2u]);  // 256-colour fg
            i += 2u;
        } else if (c == 48u && i + 2u < ncode && codes[i + 1u] == 5u) {
            cur_bg_ = static_cast<uint8_t>(codes[i + 2u]);  // 256-colour bg
            i += 2u;
        }
        // bold(1) / 38;2;r;g;b truecolour / etc. ignored
    }
}

void TerminalWidget::dispatch_csi_(char final_byte) {
    switch (final_byte) {
        case 'm':
            apply_sgr_();
            break;
        case 'A':  // CUU cursor up (param ignored; default 1 step)
            cur_row_ = (cur_row_ > 0u) ? cur_row_ - 1u : 0u;
            break;
        case 'B':  // CUD cursor down
            cur_row_ = (cur_row_ + 1u < rows_) ? cur_row_ + 1u : rows_ - 1u;
            break;
        case 'C':  // CUF cursor forward (right)
            cur_col_ = (cur_col_ + 1u < cols_) ? cur_col_ + 1u : cols_ - 1u;
            break;
        case 'D':  // CUB cursor back (left)
            cur_col_ = (cur_col_ > 0u) ? cur_col_ - 1u : 0u;
            break;
        case 'H':
        case 'f': {  // cursor position [row;colH (1-based; 0/empty treated as 1)
            uint32_t r    = 1;
            uint32_t c    = 1;
            uint32_t cur  = 0;
            bool     has  = false;
            uint8_t  slot = 0;
            for (uint8_t i = 0; i < csi_len_; ++i) {
                const char b = csi_param_[i];
                if (b >= '0' && b <= '9') {
                    cur = cur * 10u + static_cast<uint32_t>(b - '0');
                    has = true;
                } else if (b == ';') {
                    if (slot == 0u) {
                        r = has ? cur : 1u;
                    } else if (slot == 1u) {
                        c = has ? cur : 1u;
                    }
                    ++slot;
                    cur = 0;
                    has = false;
                }
            }
            if (slot == 0u) {
                r = has ? cur : 1u;
            } else if (slot == 1u) {
                c = has ? cur : 1u;
            }
            if (r == 0u) {
                r = 1u;
            }
            if (c == 0u) {
                c = 1u;
            }
            cur_row_ = (r - 1u < rows_) ? r - 1u : rows_ - 1u;
            cur_col_ = (c - 1u < cols_) ? c - 1u : cols_ - 1u;
            break;
        }
        case 'J': {  // [J erase display: 0/empty=cursor-to-end, 1=start-to-cursor, 2=whole
            uint32_t n   = 0;
            bool     has = false;
            for (uint8_t i = 0; i < csi_len_; ++i) {
                if (csi_param_[i] >= '0' && csi_param_[i] <= '9') {
                    n   = n * 10u + static_cast<uint32_t>(csi_param_[i] - '0');
                    has = true;
                }
            }
            const uint32_t mode = has ? n : 0u;
            auto           clear_row_seg = [&](uint32_t r, uint32_t c0, uint32_t c1) {
                for (uint32_t c = c0; c < c1 && c < cols_; ++c) {
                    cells_[r * kMaxCols + c] = 0;
                }
            };
            if (mode == 0u) {
                // cursor to end of screen: rest of current row (cur_col..cols) +
                // all rows below. MUST start at cur_col_ so a line-edit
                // "\b ESC[J" backspace leaves the prompt (left of cursor) intact.
                clear_row_seg(cur_row_, cur_col_, cols_);
                for (uint32_t r = cur_row_ + 1u; r < rows_; ++r) clear_row_seg(r, 0u, cols_);
            } else if (mode == 1u) {
                // start of screen to cursor
                for (uint32_t r = 0u; r < cur_row_; ++r) clear_row_seg(r, 0u, cols_);
                clear_row_seg(cur_row_, 0u, cur_col_ + 1u);
            } else {  // mode 2: whole grid
                for (uint32_t i = 0; i < kMaxCols * kMaxRows; ++i) {
                    cells_[i] = 0;
                }
            }
            dirty_all_ = true;  // erase-display must repaint the whole grid
            invalidate();
            break;
        }
        default:
            break;  // other CSI: ignored
    }
    csi_len_ = 0;  // reset param buffer for the next CSI
}

void TerminalWidget::put_char_(char ch) {
    switch (ansi_state_) {
        case AnsiState::kEsc:
            if (ch == '[') {
                csi_len_    = 0u;
                ansi_state_ = AnsiState::kCsi;
            } else if (ch == ']') {
                ansi_state_ = AnsiState::kOsc;
            } else {
                ansi_state_ = AnsiState::kNormal;  // ESC + single byte: consumed
            }
            return;
        case AnsiState::kCsi:
            if (ch >= 0x40 && ch <= 0x7E) {  // final byte ends CSI
                ansi_state_ = AnsiState::kNormal;
                dispatch_csi_(ch);
            } else if (ch >= 0x20 && ch <= 0x3F && csi_len_ < sizeof(csi_param_)) {
                csi_param_[csi_len_++] = ch;  // collect param/intermediate bytes
            }
            return;
        case AnsiState::kOsc:
            if (ch == 0x07) {  // BEL ends OSC
                ansi_state_ = AnsiState::kNormal;
            }
            return;
        case AnsiState::kNormal:
        default:
            if (ch == 0x1B) {  // ESC starts an escape
                ansi_state_ = AnsiState::kEsc;
                return;
            }
            break;  // fall through to normal byte handling
    }

    switch (ch) {
        case '\n':
            newline_();
            return;
        case '\r':
            cur_col_ = 0;
            return;
        case '\b':  // 0x08 BS: move cursor back one column, do NOT erase.
            // ANSI BS only moves the cursor; ash line-edit shifts the cursor
            // with \b, so erasing here wiped every glyph it passed (left-arrow
            // "ate" characters).  Deletion is 0x7f (DEL) below.
            if (cur_col_ > 0) {
                --cur_col_;
            }
            return;
        case 0x7f:  // DEL: busybox line-edit sends 0x7f for backspace (delete)
            if (cur_col_ > 0) {
                --cur_col_;
                const uint32_t idx    = cur_row_ * kMaxCols + cur_col_;
                cells_[idx]           = 0;
                fg_colors_[idx]       = 0;
                bg_colors_[idx]       = 0;
                dirty_rows_[cur_row_] = true;
                dirty_self_           = true;
            }
            return;
        case '\t':
            cur_col_ = (cur_col_ / 8u + 1u) * 8u;
            if (cur_col_ >= cols_) {
                newline_();
            }
            return;
        default:
            break;
    }
    if (static_cast<uint8_t>(ch) < 0x20u) {
        return;  // other control bytes: drop
    }
    const uint32_t idx    = cur_row_ * kMaxCols + cur_col_;
    cells_[idx]           = ch;
    fg_colors_[idx]       = cur_fg_;  // P5-e: stamp the current SGR fg on this cell
    bg_colors_[idx]       = cur_bg_;  // P6-c: stamp the current SGR bg
    dirty_rows_[cur_row_] = true;     // P5-f: this row changed
    dirty_self_           = true;
    ++cur_col_;
    if (cur_col_ >= cols_) {
        newline_();  // auto-wrap at the right edge
    }
}

void TerminalWidget::write(const char* data, uint32_t len) {
    if (data == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < len; ++i) {
        put_char_(data[i]);
    }
    invalidate();  // P5-c: cells changed
}

void TerminalWidget::write(const char* str) {
    if (str == nullptr) {
        return;
    }
    for (const char* p = str; *p != '\0'; ++p) {
        put_char_(*p);
    }
    invalidate();  // P5-c: cells changed
}

void TerminalWidget::collect_dirty(Region& sink) const {
    if (!dirty_self_) {
        return;
    }
    if (dirty_all_) {
        sink.add(rect_);  // scroll/clear -> whole grid
        return;
    }
    /* P5-f: only the rows that actually changed (one kGlyphH-tall rect each). */
    const int32_t y0 = rect_.y0;
    const int32_t x0 = rect_.x0;
    const int32_t x1 = rect_.x1;
    for (uint32_t r = 0u; r < rows_; ++r) {
        if (dirty_rows_[r]) {
            const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
            sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
        }
    }
    /* Cursor footprint: always repaint the current + last-painted cursor row
     * so a moved cursor's old block is erased. The row the cursor left is not
     * otherwise dirty (writing a char marks only the new row), so without this
     * the old cursor block stays on screen as a trail. */
    const uint32_t cursor_rows[2] = {cur_row_, prev_cursor_row_};
    for (uint32_t i = 0u; i < 2u; ++i) {
        const uint32_t r = cursor_rows[i];
        if (r < rows_) {
            const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
            sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
        }
    }
}

void TerminalWidget::clear_dirty() {
    dirty_all_ = false;
    for (uint32_t r = 0u; r < kMaxRows; ++r) {
        dirty_rows_[r] = false;
    }
    prev_cursor_row_ = cur_row_;  // remember this frame's cursor row for next collect
    Widget::clear_dirty();  // dirty_self_ + children
}

void TerminalWidget::paint_to_list(PaintList& list) const {
    const uint32_t defbg = (theme_ != nullptr) ? theme_->surface : 0x00000000u;  // black

    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), defbg);
    for (uint32_t r = 0; r < rows_; ++r) {
        for (uint32_t c = 0; c < cols_; ++c) {
            const uint32_t idx = r * kMaxCols + c;
            const uint8_t  bgi = bg_colors_[idx];  // P6-c: per-cell bg
            if (bgi != 0u) {                       // non-default bg
                list.fill_rect(rect_.x0 + static_cast<int32_t>(c * kGlyphW),
                               rect_.y0 + static_cast<int32_t>(r * kGlyphH), kGlyphW, kGlyphH,
                               palette_color(bgi));
            }
            const char ch = cells_[idx];
            if (ch == 0) {
                continue;  // empty cell
            }
            const uint32_t cfg = palette_color(fg_colors_[idx]);  // P6-c: 256-colour fg
            list.text_glyph(rect_.x0 + static_cast<int32_t>(c * kGlyphW),
                            rect_.y0 + static_cast<int32_t>(r * kGlyphH), cfg, ch);
        }
    }
    /* P6-c: cursor block at (cur_col, cur_row) -- a solid white square. */
    if (cur_col_ < cols_ && cur_row_ < rows_) {
        list.fill_rect(rect_.x0 + static_cast<int32_t>(cur_col_ * kGlyphW),
                       rect_.y0 + static_cast<int32_t>(cur_row_ * kGlyphH), kGlyphW, kGlyphH,
                       0x00FFFFFFu);
    }
}

}  // namespace cinux::gui
