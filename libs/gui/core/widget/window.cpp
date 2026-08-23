/**
 * @file core/widget/window.cpp
 * @brief cinux::gui Window -- paint / layout / hit-test / drag / close (P4-a)
 *
 * Compile condition: CINUX_GUI.
 */

#include "window.hpp"

#include <stdint.h>

#include "swraster.hpp"  // kCornerTL/TR (P5-d per-corner title bar)

namespace cinux::gui {

void Window::set_content(Widget* c) {
    if (c == nullptr || content_ != nullptr) {
        return;  // null or already set -> ignore (single content slot)
    }
    content_ = c;
    add_child(c);
}

Rect Window::content_rect() const {
    const int32_t y0c = rect_.y0 + static_cast<int32_t>(kTitleBarHeight);
    return Rect{rect_.x0, y0c, rect_.x1, rect_.y1};
}

void Window::layout() {
    if (content_ == nullptr) {
        return;
    }
    const Rect cr = content_rect();
    content_->set_rect(cr.x0, cr.y0, cr.width(), cr.height());
    content_->layout();
}

void Window::move_to_(int32_t x, int32_t y) {
    const uint32_t w   = rect_.width();
    const uint32_t h   = rect_.height();
    const Rect     old = rect_;
    set_rect(x, y, w, h);
    layout();         // reposition content into the new rect
    invalidate(old);  // P5-f: old footprint (bg restore)
    invalidate();     // new footprint
}

bool Window::in_title_bar_(int32_t x, int32_t y) const {
    return rect_.contains(x, y) && y < rect_.y0 + static_cast<int32_t>(kTitleBarHeight);
}

void Window::close_button_rect_(int32_t* x, int32_t* y) const {
    *x = rect_.x1 - static_cast<int32_t>(kCloseButtonSize);
    *y = rect_.y0;
}

bool Window::in_close_button_(int32_t x, int32_t y) const {
    int32_t bx = 0;
    int32_t by = 0;
    close_button_rect_(&bx, &by);
    return x >= bx && x < bx + static_cast<int32_t>(kCloseButtonSize) && y >= by &&
           y < by + static_cast<int32_t>(kCloseButtonSize);
}

void Window::resize_handle_rect_(int32_t* x, int32_t* y) const {  // P6-b
    *x = rect_.x1 - static_cast<int32_t>(kResizeHandle);
    *y = rect_.y1 - static_cast<int32_t>(kResizeHandle);
}

bool Window::in_resize_handle_(int32_t x, int32_t y) const {  // P6-b
    int32_t hx = 0;
    int32_t hy = 0;
    resize_handle_rect_(&hx, &hy);
    return x >= hx && x < rect_.x1 && y >= hy && y < rect_.y1;
}

Widget* Window::hit_test(int32_t x, int32_t y) {
    if (!visible_ || !rect_.contains(x, y)) {
        return nullptr;
    }
    if (in_close_button_(x, y)) {
        return this;  // close button -> Window handles
    }
    if (in_resize_handle_(x, y)) {
        return this;  // P6-b: resize grip -> Window handles
    }
    if (in_title_bar_(x, y)) {
        return this;  // title-bar drag -> Window handles
    }
    if (content_ != nullptr) {
        if (Widget* h = content_->hit_test(x, y); h != nullptr) {
            return h;  // content (or its child) takes it
        }
    }
    return this;  // empty content area falls back to the window
}

void Window::on_pointer(const PointerPayload& p) {
    if (p.kind == kPointerKindDown) {
        if (in_close_button_(p.x, p.y)) {
            close_armed_ = true;
            invalidate();                          // P5-c: close button armed
        } else if (in_resize_handle_(p.x, p.y)) {  // P6-b: resize grip
            resizing_ = true;
            rw_px_    = p.x;
            rw_py_    = p.y;
            rw_ow_    = static_cast<int32_t>(rect_.width());
            rw_oh_    = static_cast<int32_t>(rect_.height());
        } else if (in_title_bar_(p.x, p.y)) {
            dragging_ = true;
            drag_px_  = p.x;
            drag_py_  = p.y;
            win_ox_   = rect_.x0;
            win_oy_   = rect_.y0;
            invalidate();
        }
    } else if (p.kind == kPointerKindMove) {
        if (dragging_) {
            move_to_(win_ox_ + (p.x - drag_px_), win_oy_ + (p.y - drag_py_));
        } else if (resizing_) {  // P6-b: drag the grip -> grow/shrink
            int32_t nw = rw_ow_ + (p.x - rw_px_);
            int32_t nh = rw_oh_ + (p.y - rw_py_);
            if (nw < 40) {
                nw = 40;
            }                                                      // min width
            if (nh < static_cast<int32_t>(kTitleBarHeight) + 4) {  // min height
                nh = static_cast<int32_t>(kTitleBarHeight) + 4;
            }
            const Rect old = rect_;
            set_rect(rect_.x0, rect_.y0, static_cast<uint32_t>(nw), static_cast<uint32_t>(nh));
            layout();
            invalidate(old);
            invalidate();
        }
    } else if (p.kind == kPointerKindUp) {
        if (close_armed_ && in_close_button_(p.x, p.y) && on_close_ != nullptr) {
            on_close_(on_close_ctx_, this);
        }
        dragging_    = false;
        resizing_    = false;
        close_armed_ = false;
        invalidate();
    }
}

void Window::paint_to_list(PaintList& list) const {
    const bool     has_theme  = (theme_ != nullptr);
    const uint32_t surface    = has_theme ? theme_->surface : 0x00FFFFFFu;
    const uint32_t primary    = has_theme ? theme_->primary : 0x006200EEu;
    const uint32_t on_primary = has_theme ? theme_->on_primary : 0x00FFFFFFu;
    const uint32_t radius     = has_theme ? theme_->card_radius : 8u;

    /* 1. Window body: rounded surface fill. The title band below overpaints the
     *    top corners, so only the bottom corners read rounded (P4-a). */
    list.fill_round_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), surface, radius);

    /* 2. Title-bar band: top corners rounded (TL|TR), bottom square -> it meets
     *    the content area cleanly instead of overpainting the body's top corners
     *    (P5-d per-corner radius). */
    list.fill_round_rect_corners(rect_.x0, rect_.y0, rect_.width(), kTitleBarHeight, primary,
                                 radius, kCornerTL | kCornerTR);

    /* 3. Title text, left-padded. */
    if (title_ != nullptr && title_[0] != '\0') {
        list.text(rect_.x0 + static_cast<int32_t>(kTitlePadX),
                  rect_.y0 + static_cast<int32_t>(kTitlePadY), on_primary, title_);
    }

    /* 4. Close button: "x" in the top-right; armed (pressed) inverts. */
    int32_t cx = 0;
    int32_t cy = 0;
    close_button_rect_(&cx, &cy);
    if (close_armed_) {
        list.fill_rect(cx, cy, kCloseButtonSize, kCloseButtonSize, on_primary);
        list.text(cx + 4, cy + 2, primary, "x");
    } else {
        list.text(cx + 4, cy + 2, on_primary, "x");
    }

    /* 5. P6-b: resize grip at the bottom-right (8x8 outline square). */
    int32_t hx = 0;
    int32_t hy = 0;
    resize_handle_rect_(&hx, &hy);
    const uint32_t outline = has_theme ? theme_->outline : 0x00888888u;
    list.fill_rect(hx, hy, kResizeHandle, kResizeHandle, outline);
}

void Window::collect_dirty(Region& sink) const {
    Widget::collect_dirty(sink);  // self dirty + children_ (content is one)
    if (content_ != nullptr) {
        content_->collect_dirty(sink);  // explicit: content dirty must reach WM
    }
}

void Window::clear_dirty() {
    Widget::clear_dirty();
    if (content_ != nullptr) {
        content_->clear_dirty();
    }
}

void Window::set_maximized(bool m, Rect full) {  // P6-b
    if (m && !maximized_) {
        prev_rect_ = rect_;
        set_rect(full.x0, full.y0, full.width(), full.height());
        maximized_ = true;
        layout();
        invalidate();
    } else if (!m && maximized_) {
        set_rect(prev_rect_.x0, prev_rect_.y0, prev_rect_.width(), prev_rect_.height());
        maximized_ = false;
        layout();
        invalidate();
    }
}

void Window::set_minimized(bool m) {  // P6-b
    minimized_ = m;
    set_visible(!m);  // hide: Widget::flatten / hit_test skip invisible
    invalidate();
}

}  // namespace cinux::gui
