/**
 * @file core/widget.cpp
 * @brief cinux::gui widget tree -- flatten / hit-test / dispatch / render
 *
 * Host-neutral, ZERO host includes. render() borrows the Compositor's execute()
 * to paint the flattened PaintList.
 *
 * Compile condition: CINUX_GUI.
 */

#include "widget.hpp"

#include <stdint.h>

#include "compositor.hpp"  // execute
#include "font.hpp"        // PsfFont
#include "swraster.hpp"    // Surface

namespace cinux::gui {

void Widget::set_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    rect_ = Rect{x, y, x + static_cast<int32_t>(w), y + static_cast<int32_t>(h)};
}

void Widget::add_child(Widget* w) {
    if (w == nullptr || child_count_ >= kMaxChildren) {
        return;  // full or null -> drop (caller keeps kMaxChildren generous)
    }
    children_[child_count_++] = w;
    w->parent_                = this;  // P5-c: so invalidate() propagates up
}

void Widget::flatten(PaintList& list) const {
    if (!visible_) {
        return;
    }
    list.clip_push(rect_.x0, rect_.y0, rect_.x1, rect_.y1);
    paint_to_list(list);  // virtual -- subclass drawing
    for (uint32_t i = 0u; i < child_count_; i++) {
        children_[i]->flatten(list);
    }
    list.clip_pop();
}

Widget* Widget::hit_test(int32_t x, int32_t y) {
    if (!visible_ || !rect_.contains(x, y)) {
        return nullptr;
    }
    // children last-to-first: later children paint on top, so they hit first
    for (uint32_t i = child_count_; i > 0u; i--) {
        if (Widget* hit = children_[i - 1u]->hit_test(x, y); hit != nullptr) {
            return hit;
        }
    }
    return this;
}

void Desktop::dispatch_pointer(const PointerPayload& p) {
    if (root_ == nullptr) {
        return;
    }
    if (p.kind == kPointerKindDown) {
        /* Press capture: the widget that got the down keeps receiving move/up,
         * so a drag stays tracked even when the pointer leaves it (Slider). */
        press_target_ = root_->hit_test(p.x, p.y);
        if (press_target_ != nullptr) {
            focus_ = press_target_;  // P6-a: keyboard focus follows the click
            press_target_->on_pointer(p);
        }
    } else if (p.kind == kPointerKindUp) {
        if (press_target_ != nullptr) {
            press_target_->on_pointer(p);
        }
        press_target_ = nullptr;  // release capture
    } else if (press_target_ != nullptr) {
        /* Move while pressed: deliver to the capture target (not re-hit-tested,
         * so dragging off the thumb still moves it). */
        press_target_->on_pointer(p);
    }
    /* Move with no press_target is ignored -- P3 has no hover. */
}

void Desktop::dispatch_key(const KeycodePayload& k) {
    if (focus_ != nullptr) {
        focus_->on_key(k);  // P6-a: deliver to the focused widget
    }
}

void Desktop::render(Surface& staging, const PsfFont& font, Region* dirty) {
    Region  local;
    Region* d = (dirty != nullptr) ? dirty : &local;
    d->clear();
    if (root_ == nullptr) {
        return;
    }
    const Rect full{0, 0, static_cast<int32_t>(staging.width),
                    static_cast<int32_t>(staging.height)};
    if (first_) {
        d->add(full);  // P5-f: first frame paints everything
        first_ = false;
    } else {
        root_->collect_dirty(*d);  // P5-f: per-widget dirty rects
    }
    if (d->empty()) {
        return;  // idle -> 0 rects -> pump flushes nothing
    }
    root_->clear_dirty();
    root_->layout();
    paint_list_.clear();
    root_->flatten(paint_list_);
    /* P7-c: the Compositor owns the cursor; ask the root (WM) where it is. */
    int32_t    cx         = 0;
    int32_t    cy         = 0;
    const bool has_cursor = root_->cursor_pos(&cx, &cy);
    comp_.set_cursor(cx, cy, has_cursor);
    const uint32_t n = d->count();
    for (uint32_t i = 0u; i < n; ++i) {  // repaint each dirty rect, clipped to it
        const Rect&    r = d->rects()[i];
        const ClipRect clip{r.x0, r.y0, r.x1, r.y1};
        comp_.render(staging, paint_list_, font, &clip);
    }
}

}  // namespace cinux::gui
