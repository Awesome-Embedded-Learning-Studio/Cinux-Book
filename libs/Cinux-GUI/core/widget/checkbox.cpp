/**
 * @file core/widget/checkbox.cpp
 * @brief CheckBox -- toggle + paint (P6-a)
 *
 * Compile condition: CINUX_GUI.
 */

#include "checkbox.hpp"

#include <stdint.h>

namespace cinux::gui {

void CheckBox::on_pointer(const PointerPayload& p) {
    if (p.kind == kPointerKindDown) {
        checked_ = !checked_;
        invalidate();
    }
}

void CheckBox::paint_to_list(PaintList& list) const {
    const uint32_t outline = (theme_ != nullptr) ? theme_->outline : 0x00888888u;
    const uint32_t surface = (theme_ != nullptr) ? theme_->surface : 0x00FFFFFFu;
    const uint32_t primary = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;

    constexpr uint32_t kBox = 16u;
    /* Vertically centre the 16px box in the widget rect. */
    const int32_t x = rect_.x0;
    const int32_t y =
        rect_.y0 + (static_cast<int32_t>(rect_.height()) - static_cast<int32_t>(kBox)) / 2;

    list.fill_rect(x, y, kBox, kBox, outline);                    // outer
    list.fill_rect(x + 1, y + 1, kBox - 2u, kBox - 2u, surface);  // hollow inside
    if (checked_) {
        list.fill_rect(x + 3, y + 3, kBox - 6u, kBox - 6u, primary);  // check fill
    }
}

}  // namespace cinux::gui
