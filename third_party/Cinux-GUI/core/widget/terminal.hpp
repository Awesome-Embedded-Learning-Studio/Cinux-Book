/**
 * @file core/widget/terminal.hpp
 * @brief cinux::gui TerminalWidget -- char-grid display for a shell (P4-c)
 *
 * A char-grid widget (cols x rows of cells) that renders like a console: each
 * non-empty cell is one glyph via PaintList::text_glyph. write() feeds bytes
 * that parse \n / \r / \b / \t / printable; row overflow scrolls up one line.
 *
 * P4-c is display-only: write() is fed by whoever owns the widget (P4-d wires
 * a PTY here). No ANSI/escape parsing yet -- pure text + the control bytes
 * above; curses-style escapes arrive later (raw PTY bytes still display as
 * glyphs or are dropped).
 *
 * Cells are a fixed kMaxCols x kMaxRows grid (rows beyond the configured count
 * are unused); the stride is kMaxCols so cols_ can shrink without re-layout.
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes.
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "../paint_list.hpp"  // PaintList
#include "../theme.hpp"       // Theme
#include "../widget.hpp"      // Widget

namespace cinux::gui {

class TerminalWidget : public Widget {
public:
    static constexpr uint32_t kDefaultCols = 80;
    static constexpr uint32_t kDefaultRows = 25;
    static constexpr uint32_t kMaxCols     = 120;
    static constexpr uint32_t kMaxRows     = 50;
    static constexpr uint32_t kGlyphW      = 8;  // bundled PSF is 8x16
    static constexpr uint32_t kGlyphH      = 16;

    /** Resize the grid (clamped to kMax*); clears all cells + resets cursor. */
    void set_cols_rows(uint32_t cols, uint32_t rows);

    void set_theme(const Theme* th) { theme_ = th; }

    /** Feed @p len bytes; parses \n / \r / \b / \t / printable. */
    void write(const char* data, uint32_t len);
    /** NUL-terminated convenience overload. */
    void write(const char* str);

    void clear();

    uint32_t cursor_col() const { return cur_col_; }
    uint32_t cursor_row() const { return cur_row_; }
    uint32_t cols() const { return cols_; }
    uint32_t rows() const { return rows_; }
    /** Cell at (col,row); 0 if empty or out of the configured grid. */
    char cell_at(uint32_t col, uint32_t row) const;
    /** Foreground colour index (0-255) of the cell at (col,row). P5-e/P6-c. */
    uint8_t fg_at(uint32_t col, uint32_t row) const;
    /** Background colour index (0-255) of the cell at (col,row). P6-c. */
    uint8_t bg_at(uint32_t col, uint32_t row) const;

protected:
    void paint_to_list(PaintList& list) const override;
    void collect_dirty(Region& sink) const override;  // P5-f: per-row dirty rects
    void clear_dirty() override;                      // P5-f: reset row flags

private:
    /* ANSI/VT100 escape consumer. P4-c swallowed sequences whole; P5-e parses
     * CSI and acts: SGR 'm' sets the fg colour, 'H' moves the cursor, 'J' clears
     * the grid. OSC (ESC ] ... BEL) and ESC + other byte are still swallowed. */
    enum class AnsiState : uint8_t { kNormal, kEsc, kCsi, kOsc };

    void put_char_(char ch);
    void newline_();
    void scroll_up_();
    void dispatch_csi_(char final_byte);  // P5-e: act on a completed CSI (m/H/J)
    void apply_sgr_();                    // P5-e: parse csi_param_ SGR codes -> cur_fg_

    char         cells_[kMaxCols * kMaxRows]     = {};
    uint8_t      fg_colors_[kMaxCols * kMaxRows] = {};     // P5-e/P6-c: per-cell fg (0-255)
    uint8_t      bg_colors_[kMaxCols * kMaxRows] = {};     // P6-c: per-cell bg (0-255)
    bool         dirty_rows_[kMaxRows]           = {};     // P5-f: per-row dirty
    bool         dirty_all_                      = false;  // P5-f: whole-grid dirty
    uint32_t     cols_                           = kDefaultCols;
    uint32_t     rows_                           = kDefaultRows;
    uint32_t     cur_col_                        = 0;
    uint32_t     cur_row_                        = 0;
    uint32_t     prev_cursor_row_                = 0;  // last-painted cursor row (erase trail)
    uint8_t      cur_fg_                         = 7u;  // current SGR fg (white)
    uint8_t      cur_bg_                         = 0u;  // P6-c: current SGR bg (black)
    const Theme* theme_                          = nullptr;
    AnsiState    ansi_state_                     = AnsiState::kNormal;
    char         csi_param_[16]                  = {};  // P5-e: collected CSI param bytes
    uint8_t      csi_len_                        = 0u;
};

}  // namespace cinux::gui
