/**
 * @file core/widget/window_manager.cpp
 * @brief cinux::gui WindowManager -- Z-order / raise / close / cursor (P4-b)
 *
 * Compile condition: CINUX_GUI.
 */

#include "window_manager.hpp"

#include <stdint.h>

namespace cinux::gui {

int32_t WindowManager::index_of_(Window* w) const {
    for (uint32_t i = 0; i < count_; ++i) {
        if (windows_[i] == w) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void WindowManager::close_cb_(void* ctx, Window* w) {
    static_cast<WindowManager*>(ctx)->remove_window(w);
}

void WindowManager::add_window(Window* w) {
    if (w == nullptr || count_ >= kMaxWindows) {
        return;
    }
    if (index_of_(w) >= 0) {
        return;  // already managed
    }
    windows_[count_++] = w;
    w->set_on_close(close_cb_, this);
    invalidate();  // P5-c: a new window appeared
}

void WindowManager::remove_window(Window* w) {
    const int32_t idx = index_of_(w);
    if (idx < 0) {
        return;
    }
    // Capture the footprint BEFORE unlinking: once the window leaves the list
    // its rect is stale, and core must repaint that area (background / windows
    // below) or the closed window's pixels stay on screen.  This was a core
    // bug -- hosts worked around it with a full-screen dirty flush each frame,
    // but the proper fix is invalidating here, mirroring add_window().
    const Rect stale = w->rect();
    for (uint32_t i = static_cast<uint32_t>(idx); i + 1 < count_; ++i) {
        windows_[i] = windows_[i + 1];
    }
    windows_[count_ - 1] = nullptr;
    --count_;
    if (press_target_ == w) {
        press_target_ = nullptr;
    }
    invalidate(stale);
    if (on_remove_cb_ != nullptr) {
        on_remove_cb_(on_remove_ctx_, w);  // host: tear down per-window state
    }
}

void WindowManager::raise(Window* w) {
    const int32_t idx = index_of_(w);
    if (idx < 0 || static_cast<uint32_t>(idx) == count_ - 1) {
        return;  // absent or already topmost
    }
    for (uint32_t i = static_cast<uint32_t>(idx); i + 1 < count_; ++i) {
        windows_[i] = windows_[i + 1];
    }
    windows_[count_ - 1] = w;
}

Widget* WindowManager::hit_test(int32_t x, int32_t y) {
    if (!visible_ || !rect_.contains(x, y)) {
        return nullptr;
    }
    /* topmost first: the last window in the list paints on top. Return the
     * whole window (not its content) so the WM drives raise/drag/close; window
     * contents in P4 have no pointer-interactive children. */
    for (uint32_t i = count_; i > 0; --i) {
        Window* w = windows_[i - 1];
        if (w->visible() && w->rect().contains(x, y)) {
            return w;
        }
    }
    return nullptr;  // desktop background
}

void WindowManager::process_pointer(const PointerPayload& p) {
    const int32_t old_cx = cursor_x_;
    const int32_t old_cy = cursor_y_;
    cursor_x_            = p.x;
    cursor_y_            = p.y;
    /* P5-f: only the cursor's old + new footprint, not the whole desktop.
     * Pad by 1px each side so the bitmap's outline (Compositor paints a 1px
     * border around every lit pixel) is fully covered -- else the edge
     * leaves a 1px trail on move. */
    constexpr int32_t kFootprintPad = 1;
    const int32_t     box           = static_cast<int32_t>(kCursorSize);
    invalidate(Rect{old_cx - kFootprintPad, old_cy - kFootprintPad,
                    old_cx + box + kFootprintPad, old_cy + box + kFootprintPad});
    invalidate(Rect{cursor_x_ - kFootprintPad, cursor_y_ - kFootprintPad,
                    cursor_x_ + box + kFootprintPad, cursor_y_ + box + kFootprintPad});

    if (p.kind == kPointerKindDown) {
        /* Windows sit on top of icons: hit-test windows first. */
        press_target_ = static_cast<Window*>(hit_test(p.x, p.y));
        if (press_target_ != nullptr) {
            raise(press_target_);
            invalidate(press_target_->rect());  // P5-f: Z-order changed -> repaint
            press_target_->on_pointer(p);       // arm close / begin drag
        } else {
            /* F13-B: no window hit -- try a desktop icon (press capture). */
            icon_target_ = hit_test_icon_(p.x, p.y);
            if (icon_target_ != nullptr) {
                icon_target_->on_pointer(p);  // arm; activate on up
            }
        }
    } else if (p.kind == kPointerKindUp) {
        if (press_target_ != nullptr) {
            press_target_->on_pointer(p);  // may fire on_close -> remove_window
        }
        press_target_ = nullptr;
        if (icon_target_ != nullptr) {
            icon_target_->on_pointer(p);  // may fire on_activate
            icon_target_ = nullptr;
        }
    } else {  // move: deliver to whichever capture target is active
        if (press_target_ != nullptr) {
            press_target_->on_pointer(p);
        } else if (icon_target_ != nullptr) {
            icon_target_->on_pointer(p);
        }
    }
}

void WindowManager::paint_to_list(PaintList& list) const {
    /* 1. Desktop background -- single source of truth: theme.background
     *    (no separate bg_ colour; the WM reads it straight from the theme). */
    const uint32_t bg = (theme_ != nullptr) ? theme_->background : 0x00000000u;
    list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), bg);

    /* 2. Desktop icons (bg-level; windows can occlude them). F13-B. */
    for (uint32_t i = 0u; i < icon_count_; ++i) {
        icons_[i]->flatten(list);
    }

    /* 3. Windows bottom -> top (each flattens itself + its content). */
    for (uint32_t i = 0; i < count_; ++i) {
        windows_[i]->flatten(list);
    }

    /* P7-c: cursor is now painted by the Compositor (it reads cursor_pos()),
     * not here -- the cursor lives in the compositor's state, on top of everything. */
}

void WindowManager::collect_dirty(Region& sink) const {
    if (dirty_self_) {
        sink.add(dirty_rect_);
    }
    /* F13-B: icons live in their own array (same as windows_), NOT children_. */
    for (uint32_t i = 0u; i < icon_count_; ++i) {
        icons_[i]->collect_dirty(sink);
    }
    /* P5-f: windows_ live in their own array (P4-b), NOT in children_ -- recurse
     * them explicitly, same shape as paint_to_list. Without this a Window's
     * (and its content TerminalWidget's) dirty rects never reach the host. */
    for (uint32_t i = 0u; i < count_; ++i) {
        windows_[i]->collect_dirty(sink);
    }
}

void WindowManager::clear_dirty() {
    dirty_self_ = false;
    dirty_rect_ = Rect{1, 1, 0, 0};  // degenerate (empty)
    /* F13-B: icons live in their own array (same rationale as windows_). */
    for (uint32_t i = 0u; i < icon_count_; ++i) {
        icons_[i]->clear_dirty();
    }
    /* Mirror collect_dirty: windows_ are NOT in children_, so the base
     * Widget::clear_dirty (which only recurses children_) would skip every
     * Window. That left each Window's dirty_rect_ accumulating via rect_union
     * across frames -- the bbox only grows, so once a window is dragged
     * off-screen the bbox picks up a negative origin and never shrinks back,
     * turning every later frame's window update stale. */
    for (uint32_t i = 0u; i < count_; ++i) {
        windows_[i]->clear_dirty();
    }
}

void WindowManager::add_icon(DesktopIcon* icon) {
    if (icon == nullptr || icon_count_ >= kMaxIcons) {
        return;
    }
    icons_[icon_count_++] = icon;
    invalidate(icon->rect());  // icon appeared -> repaint its footprint
}

DesktopIcon* WindowManager::hit_test_icon_(int32_t x, int32_t y) const {
    /* topmost (last-registered) first so later icons win on overlap */
    for (uint32_t i = icon_count_; i > 0u; --i) {
        DesktopIcon* ic = icons_[i - 1u];
        if (ic->visible() && ic->rect().contains(x, y)) {
            return ic;
        }
    }
    return nullptr;
}

}  // namespace cinux::gui
