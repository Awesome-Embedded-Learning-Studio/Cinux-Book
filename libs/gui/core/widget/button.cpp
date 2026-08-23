/**
 * @file core/widget/button.cpp
 * @brief Button widget -- Material contained button + press state (P3-c)
 *
 * Compile condition: CINUX_GUI.
 */

#include "button.hpp"

#include <stdint.h>

namespace cinux::gui {

void Button::paint_to_list(PaintList& list) const {
    const uint32_t primary = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;
    const uint32_t on_prim = (theme_ != nullptr) ? theme_->on_primary : 0x00FFFFFFu;
    const uint32_t surface = (theme_ != nullptr) ? theme_->surface : 0x00FFFFFFu;
    const uint32_t radius  = (theme_ != nullptr) ? theme_->button_radius : 8u;

    /* Contained Material button. Rest = primary + on_primary text; pressed =
     * surface + primary text (flat subset -- no elevation shadow/ripple). */
    if (pressed_) {
        list.fill_round_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), surface, radius);
        if (text_ != nullptr && text_[0] != '\0') {
            list.text(rect_.x0 + 8, rect_.y0 + 8, primary, text_);
        }
    } else {
        list.fill_round_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), primary, radius);
        if (text_ != nullptr && text_[0] != '\0') {
            list.text(rect_.x0 + 8, rect_.y0 + 8, on_prim, text_);
        }
    }
}

void Button::on_pointer(const PointerPayload& p) {
    if (p.kind == kPointerKindDown) {
        pressed_ = true;
        invalidate();  // P5-c: pressed state changed
    } else if (p.kind == kPointerKindUp) {
        pressed_ = false;
        invalidate();
    }
}

}  // namespace cinux::gui
