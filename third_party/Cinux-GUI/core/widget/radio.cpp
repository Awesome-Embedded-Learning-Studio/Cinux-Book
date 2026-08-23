/**
 * @file core/widget/radio.cpp
 * @brief Radio + RadioGroup -- exclusive select + paint (P7-a)
 *
 * Compile condition: CINUX_GUI.
 */

#include "radio.hpp"

#include <stdint.h>

namespace cinux::gui {

void RadioGroup::add(Radio* r) {
    if (r != nullptr && count_ < kMaxRadios) {
        radios_[count_++] = r;
    }
}

void RadioGroup::select(Radio* r) {
    for (uint32_t i = 0; i < count_; ++i) {
        radios_[i]->set_checked(radios_[i] == r);
    }
    selected_ = r;
}

void Radio::set_checked(bool c) {
    if (checked_ != c) {
        checked_ = c;
        invalidate();
    }
}

void Radio::on_pointer(const PointerPayload& p) {
    if (p.kind == kPointerKindDown && group_ != nullptr) {
        group_->select(this);  // exclusive: this one on, the rest off
    }
}

void Radio::paint_to_list(PaintList& list) const {
    const uint32_t outline = (theme_ != nullptr) ? theme_->outline : 0x00888888u;
    const uint32_t surface = (theme_ != nullptr) ? theme_->surface : 0x00FFFFFFu;
    const uint32_t primary = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;

    constexpr uint32_t kDot = 16u;
    /* Vertically centre the 16px dot in the widget rect (pairs with a Label). */
    const int32_t x = rect_.x0;
    const int32_t y =
        rect_.y0 + (static_cast<int32_t>(rect_.height()) - static_cast<int32_t>(kDot)) / 2;

    /* Outer ring + hollow inside (a rounded square reads as a circle-ish dot). */
    list.fill_round_rect(x, y, kDot, kDot, outline, 8u);
    list.fill_round_rect(x + 1, y + 1, kDot - 2u, kDot - 2u, surface, 7u);
    if (checked_) {
        list.fill_round_rect(x + 4, y + 4, kDot - 8u, kDot - 8u, primary, 4u);  // filled dot
    }
}

}  // namespace cinux::gui
