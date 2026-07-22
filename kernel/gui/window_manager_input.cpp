/**
 * @file kernel/gui/window_manager_input.cpp
 * @brief WindowManager input dispatch -- handle_mouse / handle_key
 *
 * Split out of window_manager.cpp (F13: that file exceeded the 500-line limit
 * after the §4c dirty-region methods landed). Input dispatch is a distinct
 * concern from window lifecycle/state, so it gets its own translation unit.
 * These are member-function DEFINITIONS of WindowManager (declared in
 * window_manager.hpp); C++ permits a class's methods to be defined across
 * multiple .cpp files.
 *
 * Compile condition: CINUX_GUI.
 */

#include "window_manager.hpp"

namespace cinux::gui {

// ============================================================
// Input handling
// ============================================================

void WindowManager::handle_mouse(Event& ev) {
    // Update tracked mouse position
    mouse_x_ = ev.mouse.x;
    mouse_y_ = ev.mouse.y;

    switch (ev.type_) {
    case EventType::MouseDown: {
        // Only handle left button press
        if (!ev.mouse.left) {
            break;
        }

        // Hit test from top-most window downward
        Window* hit = hit_test(ev.mouse.x, ev.mouse.y);

        if (hit == nullptr) {
            // No window hit -- check if a desktop icon was clicked
            const DesktopIcon* icon_hit = hit_test_icon(ev.mouse.x, ev.mouse.y);
            if (icon_hit != nullptr) {
                pending_icon_action_ = icon_hit->action;
                if (focused_ != nullptr) {
                    focused_->set_focused(false);
                    focused_ = nullptr;
                }
            } else {
                // Clicked on the desktop background -- clear focus
                if (focused_ != nullptr) {
                    focused_->set_focused(false);
                    focused_ = nullptr;
                }
            }
            break;
        }

        int32_t local_y = ev.mouse.y - hit->y();
        int     is_title_bar =
            (local_y >= 0 && local_y < static_cast<int32_t>(Window::TITLE_BAR_HEIGHT)) ? 1 : 0;

        // Check if the close button was hit
        if (hit->is_close_button_hit(ev.mouse.x, ev.mouse.y)) {
            uint32_t dead_id = hit->id();
            destroy(dead_id);
            invalidate_all(); /* z-order/focus changed -> full re-flush */
            break;
        }

        // Raise the clicked window to the top
        raise(hit->id());

        // Check if the click landed on the title bar
        // The title bar spans from window y_ to y_ + TITLE_BAR_HEIGHT
        if (is_title_bar) {
            // Begin dragging: record the offset from window origin
            dragging_      = true;
            drag_offset_x_ = ev.mouse.x - hit->x();
            drag_offset_y_ = ev.mouse.y - hit->y();
        }

        invalidate_all(); /* raise changes z-order -> full re-flush */
        break;
    }

    case EventType::MouseMove: {
        if (dragging_ && focused_ != nullptr) {
            // Move the focused window to follow the cursor. The window's
            // off-screen canvas (title bar + content) is PERSISTENT -- only its
            // screen position changes, so do NOT repaint it here: composite()
            // blits the existing canvas to the new position. The old code called
            // draw_title_bar()+draw_content() on every move, and draw_content()
            // -> on_paint -> render_to_canvas re-rendered the whole terminal
            // (2000 cells, pixel-by-pixel) each frame even though the content
            // never changed -- that redundant re-render was the drag stall.
            int32_t new_x = ev.mouse.x - drag_offset_x_;
            int32_t new_y = ev.mouse.y - drag_offset_y_;
            focused_->set_position(new_x, new_y);

            invalidate_all(); /* window moved -> full re-flush (exposure-safe) */
        }
        break;
    }

    case EventType::MouseUp: {
        if (dragging_) {
            dragging_ = false;
        }
        break;
    }

    default:
        break;
    }
}

void WindowManager::handle_key(Event& ev) {
    // Forward keyboard events to the focused window, if any
    if (focused_ != nullptr) {
        focused_->on_key(ev.key);
    }
}

}  // namespace cinux::gui
