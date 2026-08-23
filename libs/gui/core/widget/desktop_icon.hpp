/**
 * @file core/widget/desktop_icon.hpp
 * @brief DesktopIcon -- clickable 32x32 bitmap icon with a text label (F13-B)
 *
 * A desktop icon: a w*h pixel bitmap blitted via a 1-bpp alpha mask, plus a
 * text label drawn below it. Click (pointer down then up, both inside the icon
 * rect) fires an on_activate callback -- the host wires that to "open shell",
 * "open calculator", etc.
 *
 * Paint strategy: PaintList has no blit cmd yet, so paint_to_list emits one
 * 1x1 fill_rect per opaque mask pixel (same style as the Compositor cursor).
 * Icons are static and rarely dirtied, so this path is cheap enough; the clip
 * stack pushed by Widget::flatten culls any pixels outside the icon rect.
 *
 * Pure C++17 (stdint only), ZERO host includes.
 *
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "../event_payload.hpp"  // PointerPayload
#include "../paint_list.hpp"     // PaintList
#include "../widget.hpp"         // Widget

namespace cinux::gui {

class DesktopIcon : public Widget {
public:
    using ActivateFn = void (*)(void* ctx, DesktopIcon* self);

    /** Bitmap colour pixels (w*h uint32 XRGB8888) + 1-bpp alpha mask + size. */
    void set_bitmap(const uint32_t* pixels, const uint8_t* mask, uint32_t w, uint32_t h);
    /** Label drawn below the bitmap (borrowed; caller owns). */
    void set_label(const char* label) { label_ = label; }
    /** Label colour (XRGB8888). */
    void set_label_color(uint32_t c) { label_color_ = c; }
    /** Click callback: down + up inside the rect. */
    void set_on_activate(ActivateFn fn, void* ctx) {
        on_activate_     = fn;
        on_activate_ctx_ = ctx;
    }

    /* on_pointer stays public (matches Widget's access) so the WindowManager
     * can dispatch pointer events to a DesktopIcon* it holds. */
    void on_pointer(const PointerPayload& p) override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    const uint32_t* pixels_          = nullptr;
    const uint8_t*  mask_            = nullptr;
    uint32_t        bmp_w_           = 0;
    uint32_t        bmp_h_           = 0;
    const char*     label_           = nullptr;
    uint32_t        label_color_     = 0x00FFFFFFu;
    ActivateFn      on_activate_     = nullptr;
    void*           on_activate_ctx_ = nullptr;
    bool            armed_           = false;  // down seen, waiting for up
};

}  // namespace cinux::gui
