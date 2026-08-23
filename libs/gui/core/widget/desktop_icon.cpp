/**
 * @file core/widget/desktop_icon.cpp
 * @brief DesktopIcon -- masked bitmap blit + label + click activate (F13-B)
 *
 * Namespace: cinux::gui
 */

#include "desktop_icon.hpp"

#include <stdint.h>

namespace cinux::gui {

void DesktopIcon::set_bitmap(const uint32_t* pixels, const uint8_t* mask, uint32_t w, uint32_t h) {
    pixels_ = pixels;
    mask_   = mask;
    bmp_w_  = w;
    bmp_h_  = h;
}

void DesktopIcon::paint_to_list(PaintList& list) const {
    if (pixels_ == nullptr || mask_ == nullptr || bmp_w_ == 0u || bmp_h_ == 0u) {
        return;
    }
    /* Per-pixel blit: one 1x1 fill_rect per opaque mask pixel. No PaintList
     * blit cmd yet; this mirrors the Compositor cursor paint style. Widget::
     * flatten already clip-pushed to this icon's rect, so fill_rects falling
     * outside are culled downstream. */
    const uint32_t row_bytes = (bmp_w_ + 7u) / 8u;  // 1 bpp, MSB-first per row
    for (uint32_t row = 0u; row < bmp_h_; ++row) {
        for (uint32_t col = 0u; col < bmp_w_; ++col) {
            const uint8_t  byte    = mask_[row * row_bytes + col / 8u];
            const uint32_t bit_idx = 7u - (col % 8u);
            if ((byte & static_cast<uint8_t>(1u << bit_idx)) == 0u) {
                continue;  // transparent
            }
            const uint32_t color = pixels_[row * bmp_w_ + col];
            list.fill_rect(rect_.x0 + static_cast<int32_t>(col),
                           rect_.y0 + static_cast<int32_t>(row), 1u, 1u, color);
        }
    }
    if (label_ != nullptr && label_[0] != '\0') {
        list.text(rect_.x0, rect_.y0 + static_cast<int32_t>(bmp_h_) + 2, label_color_, label_);
    }
}

void DesktopIcon::on_pointer(const PointerPayload& p) {
    if (on_activate_ == nullptr) {
        return;
    }
    if (p.kind == kPointerKindDown && rect_.contains(p.x, p.y)) {
        armed_ = true;
    } else if (p.kind == kPointerKindUp) {
        const bool was_armed = armed_;
        armed_               = false;
        /* Click = down then up, both inside the rect. The WM routes up to the
         * icon that got the down (press capture), but a press dragged off the
         * icon and released elsewhere must not fire -- verify up is inside. */
        if (was_armed && rect_.contains(p.x, p.y)) {
            on_activate_(on_activate_ctx_, this);
        }
    }
}

}  // namespace cinux::gui
