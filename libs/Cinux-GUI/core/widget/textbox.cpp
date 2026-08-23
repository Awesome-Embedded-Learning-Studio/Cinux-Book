/**
 * @file core/widget/textbox.cpp
 * @brief TextBox -- key insert/delete + paint (P6-a)
 *
 * Compile condition: CINUX_GUI.
 */

#include "textbox.hpp"

#include <stdint.h>

namespace cinux::gui {

void TextBox::on_key(const KeycodePayload& k) {
    const char ch = k.ascii;
    if (ch >= 0x20 && len_ < kMaxLen) {
        /* Insert at cursor: shift the tail right one slot. */
        for (uint32_t i = len_; i > cursor_; --i) {
            text_[i] = text_[i - 1u];
        }
        text_[cursor_] = ch;
        ++len_;
        ++cursor_;
        text_[len_] = '\0';
        invalidate();
    } else if (ch == '\b' && cursor_ > 0u) {
        /* Delete before cursor: shift the tail left one slot. */
        for (uint32_t i = cursor_ - 1u; i + 1u < len_; ++i) {
            text_[i] = text_[i + 1u];
        }
        --len_;
        --cursor_;
        text_[len_] = '\0';
        invalidate();
    }
}

void TextBox::paint_to_list(PaintList& list) const {
    const uint32_t bg      = (theme_ != nullptr) ? theme_->surface : 0x00FFFFFFu;
    const uint32_t fg      = (theme_ != nullptr) ? theme_->on_surface : 0x00181818u;
    const uint32_t primary = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;

    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), bg);
    if (len_ > 0u) {
        list.text(rect_.x0 + 4, rect_.y0 + 4, fg, text_);
    }
    /* 2px cursor block at the cursor column. */
    const int32_t cx = rect_.x0 + 4 + static_cast<int32_t>(cursor_ * kGlyphW);
    list.fill_rect(cx, rect_.y0 + 4, 2, 16, primary);
}

}  // namespace cinux::gui
