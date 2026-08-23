/**
 * @file core/widget/textbox.hpp
 * @brief TextBox -- single-line text input with a cursor (P6-a)
 *
 * Receives keyboard via on_key (Desktop routes to the focused widget; clicking
 * the box gives it focus). Printable ascii inserts at the cursor; '\b' deletes
 * before it. Paints a surface bg + the text + a 2px cursor block at the cursor
 * column (cursor * 8px glyph width). Fixed buffer, no <string>.
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes.
 * Compile condition: CINUX_GUI.
 */
#pragma once

#include <stdint.h>

#include "../event_payload.hpp"  // KeycodePayload
#include "../paint_list.hpp"     // PaintList
#include "../theme.hpp"          // Theme
#include "../widget.hpp"         // Widget

namespace cinux::gui {

class TextBox : public Widget {
public:
    static constexpr uint32_t kMaxLen = 64;
    static constexpr uint32_t kGlyphW = 8;  // PSF glyph width for cursor math

    void set_theme(const Theme* th) { theme_ = th; }

    const char* text() const { return text_; }
    uint32_t    length() const { return len_; }
    uint32_t    cursor() const { return cursor_; }
    void        on_key(const KeycodePayload& k) override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    char         text_[kMaxLen + 1] = {};
    uint32_t     len_               = 0;
    uint32_t     cursor_            = 0;
    const Theme* theme_             = nullptr;
};

}  // namespace cinux::gui
