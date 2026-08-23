/**
 * @file core/widget/window_manager.hpp
 * @brief cinux::gui WindowManager -- multi-window desktop with Z-order and a
 *        software cursor (P4-b)
 *
 * The WindowManager is the desktop root: it owns an ordered list of Windows
 * (bottom-first, so windows_[count-1] is topmost), routes pointer input with
 * press capture + click-to-raise, and paints a background + the windows in
 * Z-order + a software cursor on top.
 *
 * It does NOT use Widget's children_ array for windows. The framework flatten
 * recurses children_, which cannot express "cursor paints after all windows"
 * (paint_to_list runs before children). So WindowManager keeps its own
 * windows_ list and paints everything in paint_to_list in the right order:
 * background, then each window via Window::flatten (bottom -> top), then the
 * cursor. children_ stays empty.
 *
 * close-on-x: add_window registers the WM as the window's on_close target;
 * the callback removes the window from the list (it is not deleted -- the
 * caller owns it). Destroy-on-close is therefore "hide + drop from Z-order".
 *
 * Drag is already handled by each Window (P4-a, via Desktop-style press
 * capture); the WM's press_target_ keeps a drag going to the window that was
 * pressed even if the pointer leaves it.
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes.
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "../event_payload.hpp"  // PointerPayload
#include "../paint_list.hpp"     // PaintList
#include "../theme.hpp"          // Theme
#include "../widget.hpp"         // Widget
#include "desktop_icon.hpp"      // DesktopIcon (desktop icons, F13-B)
#include "window.hpp"            // Window

namespace cinux::gui {

class WindowManager : public Widget {
public:
    static constexpr uint32_t kMaxWindows = 16;
    /* Cursor footprint edge length. Must match the 16x16 arrow bitmap
     * Compositor::render paints; a size mismatch trails on move (only the
     * top-left corner repaints -> faint blob + smearing). */
    static constexpr uint32_t kCursorSize = 16;

    void set_theme(const Theme* th) { theme_ = th; }

    /** Register a callback fired after remove_window() unlinks a window, so the
     * host can tear down per-window state (e.g. close the shell PTY so the Shell
     * icon can spawn a fresh shell on the next click -- otherwise shell_activate
     * sees sh_master_fd >= 0 and silently returns, the reopen bug). */
    using RemoveCallback = void (*)(void* ctx, Window* w);
    void set_on_remove(RemoveCallback cb, void* ctx) { on_remove_cb_ = cb; on_remove_ctx_ = ctx; }

    /** Add a window on top. Registers on_close -> remove_window. */
    void add_window(Window* w);

    /** F13-B: add a desktop icon. Paint order is bg -> icons -> windows, so a
     * window can occlude an icon. Click (down+up inside) fires on_activate. */
    static constexpr uint32_t kMaxIcons = 16;
    void                      add_icon(DesktopIcon* icon);

    /** Drop a window from the Z-order (hide). No-op if not present. */
    void remove_window(Window* w);

    /** Bring a window to the top of the Z-order. No-op if not present. */
    void raise(Window* w);

    uint32_t window_count() const { return count_; }
    Window*  window_at(uint32_t i) const { return i < count_ ? windows_[i] : nullptr; }
    Window*  topmost() const { return count_ > 0 ? windows_[count_ - 1] : nullptr; }

    /** Last-known cursor position (updated by process_pointer). */
    int32_t cursor_x() const { return cursor_x_; }
    int32_t cursor_y() const { return cursor_y_; }
    void    set_cursor_visible(bool v) { cursor_visible_ = v; }

    /**
     * @brief Route a pointer event: press capture + click-to-raise + deliver
     *
     * down: hit-test topmost-first, raise the hit window, deliver; the hit
     * window becomes the press target so move/up keep routing to it (drag).
     * move/up: deliver to the press target. Always tracks the cursor position.
     */
    void process_pointer(const PointerPayload& p);

    /** P7-c: the Compositor paints the cursor from this position (not WM paint). */
    bool cursor_pos(int32_t* x, int32_t* y) const override {
        *x = cursor_x_;
        *y = cursor_y_;
        return cursor_visible_;
    }

protected:
    void    paint_to_list(PaintList& list) const override;
    void    collect_dirty(Region& sink) const override;  // P5-f: recurse windows_ (not children_)
    void    clear_dirty() override;                      // P5-f: recurse windows_ (mirror collect_dirty)
    Widget* hit_test(int32_t x, int32_t y) override;

private:
    static void close_cb_(void* ctx, Window* w);

    int32_t index_of_(Window* w) const;  // -1 if absent

    Window*      windows_[kMaxWindows] = {};
    uint32_t     count_                = 0;
    Window*      press_target_         = nullptr;
    const Theme* theme_                = nullptr;

    DesktopIcon* icons_[kMaxIcons] = {};  // F13-B: desktop icons (bg-level)
    uint32_t     icon_count_        = 0;
    DesktopIcon* icon_target_       = nullptr;  // press capture for icon

    DesktopIcon* hit_test_icon_(int32_t x, int32_t y) const;  // nullptr if none

    RemoveCallback on_remove_cb_  = nullptr;  // host callback after remove_window
    void*          on_remove_ctx_ = nullptr;

    int32_t cursor_x_       = 0;
    int32_t cursor_y_       = 0;
    bool    cursor_visible_ = true;
};

}  // namespace cinux::gui
