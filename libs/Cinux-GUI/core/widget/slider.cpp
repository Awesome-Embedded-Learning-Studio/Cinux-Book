/**
 * @file core/widget/slider.cpp
 * @brief Slider widget -- drag-to-value + track/thumb paint (P3-d)
 *
 * Compile condition: CINUX_GUI.
 */

#include "slider.hpp"

#include <stdint.h>

namespace cinux::gui {

void Slider::set_value(int32_t v) {
    if (v < 0) {
        v = 0;
    }
    if (v > range_) {
        v = range_;
    }
    value_ = v;
}

void Slider::apply_x_(int32_t x) {
    const int32_t pad = 4;
    const int32_t cw  = static_cast<int32_t>(rect_.width()) - 2 * pad;
    if (cw <= 0) {
        return;
    }
    int32_t v = (x - rect_.x0 - pad) * range_ / cw;
    if (v < 0) {
        v = 0;
    }
    if (v > range_) {
        v = range_;
    }
    value_ = v;
}

void Slider::on_pointer(const PointerPayload& p) {
    if (p.kind == kPointerKindDown) {
        dragging_ = true;
        apply_x_(p.x);
        invalidate();  // P5-c: thumb position changed
    } else if (p.kind == kPointerKindMove) {
        if (dragging_) {
            apply_x_(p.x);
            invalidate();
        }
    } else if (p.kind == kPointerKindUp) {
        dragging_ = false;
        invalidate();
    }
}

void Slider::paint_to_list(PaintList& list) const {
    const uint32_t track_col = (theme_ != nullptr) ? theme_->outline : 0x00BDBDBDu;
    const uint32_t primary   = (theme_ != nullptr) ? theme_->primary : 0x006200EEu;

    const int32_t  pad     = 4;
    const uint32_t track_h = 4;
    const int32_t  cw      = static_cast<int32_t>(rect_.width()) - 2 * pad;
    const int32_t  ty      = rect_.y0 + static_cast<int32_t>(rect_.height() - track_h) / 2;
    list.fill_round_rect(rect_.x0 + pad, ty, cw, track_h, track_col, 2u);

    /* Thumb: round (radius = size/2), centred on the value's x position. */
    const int32_t  denom   = (range_ != 0) ? range_ : 1;
    const int32_t  tx      = rect_.x0 + pad + cw * value_ / denom;
    const uint32_t thumb_s = 16u;
    const int32_t  thx     = tx - static_cast<int32_t>(thumb_s) / 2;
    const int32_t  thy     = rect_.y0 + static_cast<int32_t>(rect_.height() - thumb_s) / 2;
    list.fill_round_rect(thx, thy, thumb_s, thumb_s, primary, thumb_s / 2u);
}

}  // namespace cinux::gui
