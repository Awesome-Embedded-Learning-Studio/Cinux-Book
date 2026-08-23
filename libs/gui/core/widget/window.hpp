/**
 * @file core/widget/window.hpp
 * @brief cinux::gui Window -- framed top-level window with a draggable title
 *        bar and a close button (P4-a)
 *
 * A Window is a composite Widget: it paints a rounded Material body + a solid
 * title-bar band (title text + an "x" close button) and hosts one content
 * Widget below the title bar.
 *
 * Interaction:
 *   - Press inside the title bar (outside the close button) and drag: the whole
 *     window moves by the drag delta. Desktop press capture (P3-d) keeps move
 *     events routed to the Window for the whole drag, even off the title bar.
 *   - Press AND release inside the close button: fires the on_close callback.
 *
 * Why the title bar is painted by the Window itself, not an HBox[Label + Button]
 * child: P3's HBox divides children *equally* (see container.hpp), which cannot
 * express "title fills the bar, close button hugs the corner". P5 flex layout
 * would let this be a real HBox; until then the Window owns title-bar geometry
 * and hit regions. Likewise the body is a 4-corner fill_round_rect and the
 * rectangular title band overpaints the top corners, so the window reads
 * "square top, rounded bottom" until swraster gains per-corner radius (P5).
 *
 * Drag/move is self-contained here (single window). Multi-window Z-order,
 * click-to-raise, and destroy-on-close arrive with WindowManager in P4-b.
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

namespace cinux::gui {

class Window : public Widget {
public:
    /** Notified when the close button is clicked (press AND release inside it). */
    using CloseCallback = void (*)(void* ctx, Window* w);

    static constexpr uint32_t kTitleBarHeight  = 20;
    static constexpr uint32_t kCloseButtonSize = 20;
    static constexpr uint32_t kTitlePadX       = 8;
    static constexpr uint32_t kTitlePadY       = 4;
    static constexpr uint32_t kResizeHandle    = 8;  // P6-b: bottom-right resize grip

    void set_title(const char* t) { title_ = t; }
    void set_theme(const Theme* th) { theme_ = th; }

    /** Install the content widget (also added as a child for paint/hit-test). */
    void set_content(Widget* c);

    void set_on_close(CloseCallback cb, void* ctx = nullptr) {
        on_close_     = cb;
        on_close_ctx_ = ctx;
    }

    /** Rect of the content area (below the title bar); valid after layout(). */
    Rect content_rect() const;

    /** P6-b: maximize -> store current rect + fill @p full; false restores. */
    void set_maximized(bool m, Rect full);
    bool maximized() const { return maximized_; }
    /** P6-b: minimize -> hide (parent stops painting + hit-testing it). */
    void set_minimized(bool m);
    bool minimized() const { return minimized_; }

    /* Overrides kept public to match Widget's access: Desktop drives layout /
     * hit_test via Widget*, but tests and hosts also call them directly on a
     * Window, which needs public access (overriding cannot widen from public). */
    void    layout() override;
    Widget* hit_test(int32_t x, int32_t y) override;
    void    on_pointer(const PointerPayload& p) override;
    /* collect_dirty/clear_dirty stay public (Widget's are public; the WM calls
     * them via Window*). Overriding cannot narrow from public. */
    void    collect_dirty(Region& sink) const override;  // recurse content_ explicitly
    void    clear_dirty() override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    void move_to_(int32_t x, int32_t y);
    bool in_title_bar_(int32_t x, int32_t y) const;
    bool in_close_button_(int32_t x, int32_t y) const;
    void close_button_rect_(int32_t* x, int32_t* y) const;
    bool in_resize_handle_(int32_t x, int32_t y) const;      // P6-b
    void resize_handle_rect_(int32_t* x, int32_t* y) const;  // P6-b

    const char*   title_        = "";
    const Theme*  theme_        = nullptr;
    Widget*       content_      = nullptr;
    CloseCallback on_close_     = nullptr;
    void*         on_close_ctx_ = nullptr;

    /* Drag state (title-bar press). */
    bool    dragging_ = false;
    int32_t drag_px_  = 0;
    int32_t drag_py_  = 0;
    int32_t win_ox_   = 0;
    int32_t win_oy_   = 0;

    /* Close-button press state. */
    bool close_armed_ = false;

    /* P6-b: resize state (bottom-right grip press). */
    bool    resizing_ = false;
    int32_t rw_px_    = 0;  // press origin
    int32_t rw_py_    = 0;
    int32_t rw_ow_    = 0;  // window size at press
    int32_t rw_oh_    = 0;
    /* P6-b: maximize / minimize. */
    bool maximized_ = false;
    Rect prev_rect_ = Rect{1, 1, 0, 0};
    bool minimized_ = false;
};

}  // namespace cinux::gui
