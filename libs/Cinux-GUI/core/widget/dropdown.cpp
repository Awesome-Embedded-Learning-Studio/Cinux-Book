/**
 * @file core/widget/dropdown.cpp
 * @brief Dropdown -- open/select + paint (P7-a)
 *
 * Compile condition: CINUX_GUI.
 */

#include "dropdown.hpp"

#include <stdint.h>

namespace cinux::gui {

void Dropdown::set_option(uint32_t i, const char* text) {
    if (i < kMaxOpts) {
        options_[i] = text;
    }
}

void Dropdown::set_option_count(uint32_t n) { opt_count_ = (n > kMaxOpts) ? kMaxOpts : n; }

const char* Dropdown::selected_text() const {
    return (selected_ < opt_count_) ? options_[selected_] : nullptr;
}

uint32_t Dropdown::row_at_(int32_t y) const {
    if (y < rect_.y0) {
        return kMaxOpts;  // out of range
    }
    return static_cast<uint32_t>((y - rect_.y0) / static_cast<int32_t>(kRowH));
}

void Dropdown::on_pointer(const PointerPayload& p) {
    if (p.kind != kPointerKindDown) {
        return;
    }
    const uint32_t row = row_at_(p.y);
    if (!expanded_) {
        expanded_ = true;  // first click opens the list
    } else if (row < opt_count_) {
        selected_ = row;  // click a row -> pick + close
        expanded_ = false;
    } else {
        expanded_ = false;  // click outside the rows -> just close
    }
    invalidate();
}

Widget* Dropdown::hit_test(int32_t x, int32_t y) {
    if (!visible_ || !rect_.contains(x, y)) {
        return nullptr;
    }
    return this;
}

void Dropdown::paint_to_list(PaintList& list) const {
    const uint32_t surface    = (theme_ != nullptr) ? theme_->surface : 0x00FFFFFFu;
    const uint32_t on_surface = (theme_ != nullptr) ? theme_->on_surface : 0x00181818u;
    const uint32_t primary    = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;
    const uint32_t on_primary = (theme_ != nullptr) ? theme_->on_primary : 0x00FFFFFFu;

    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), surface);

    /* Closed paints only the top (selected) row; open paints every option. */
    const uint32_t rows = expanded_ ? opt_count_ : 1u;
    for (uint32_t i = 0u; i < rows && i < opt_count_; ++i) {
        const int32_t ry  = rect_.y0 + static_cast<int32_t>(i * kRowH);
        const bool    sel = (i == selected_);
        if (sel) {  // highlight the selected row
            list.fill_rect(rect_.x0, ry, rect_.width(), kRowH, primary);
        }
        if (options_[i] != nullptr) {
            list.text(rect_.x0 + 4, ry + 2, sel ? on_primary : on_surface, options_[i]);
        }
    }

    /* "v" marker at the right of the closed row (no triangle glyph in PSF). */
    if (!expanded_ && opt_count_ > 0u) {
        list.text(rect_.x1 - 8, rect_.y0 + 2, on_surface, "v");
    }
}

}  // namespace cinux::gui
