/**
 * @file core/widget/label.cpp
 * @brief Label widget -- optional bg + left/top-padded text (P3-c)
 *
 * Compile condition: CINUX_GUI.
 */

#include "label.hpp"

#include <stdint.h>

namespace cinux::gui {

void Label::paint_to_list(PaintList& list) const {
    if (bg_ != 0u) {
        list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), bg_);
    }
    if (text_ != nullptr && text_[0] != '\0') {
        if (scale_ > 1u) {
            list.text_scaled(rect_.x0 + 4, rect_.y0 + 4, color_, text_, scale_);
        } else {
            list.text(rect_.x0 + 4, rect_.y0 + 4, color_, text_);
        }
    }
}

}  // namespace cinux::gui
