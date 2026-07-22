/**
 * @file kernel/gui/window_manager.cpp
 * @brief WindowManager class implementation
 */

#include "window_manager.hpp"

#include "drivers/mouse.hpp"
#include "drivers/video/font.hpp"

namespace cinux::gui {

// ============================================================
// Singleton
// ============================================================

WindowManager& WindowManager::instance() {
    static WindowManager wm;
    return wm;
}

// ============================================================
// Construction / destruction
// ============================================================

WindowManager::~WindowManager() {
    // Destroy all remaining windows
    for (uint32_t i = 0; i < count_; i++) {
        delete windows_[i];
        windows_[i] = nullptr;
    }
    count_   = 0;
    focused_ = nullptr;
}

// ============================================================
// Lifecycle
// ============================================================

void WindowManager::init(cinux::drivers::Canvas* screen, cinux::drivers::PSFFont* font) {
    screen_              = screen;
    font_                = font;
    count_               = 0;
    focused_             = nullptr;
    dragging_            = false;
    mouse_x_             = 0;
    mouse_y_             = 0;
    icon_count_          = 0;
    pending_icon_action_ = IconAction::None;
}

// ============================================================
// Window management
// ============================================================

uint32_t WindowManager::create(const char* title, uint32_t w, uint32_t h) {
    // Reject if we have reached the maximum window count
    if (count_ >= MAX_WINDOWS) {
        return 0;
    }

    // Stagger new windows so they are not perfectly overlapping
    int32_t offset_x = static_cast<int32_t>(count_ * 30);
    int32_t offset_y = static_cast<int32_t>(count_ * 30);

    // Heap-allocate the new window at the top of Z-order
    windows_[count_] = new Window(title, offset_x, offset_y, w, h);

    // Draw the title bar and content onto the window's off-screen canvas
    if (font_ != nullptr) {
        windows_[count_]->draw_title_bar(*font_);
    }
    windows_[count_]->draw_content();

    count_++;

    // Update focus to the new top-most window
    update_focus();

    return windows_[count_ - 1]->id();
}

uint32_t WindowManager::add_window(Window* win) {
    // Reject if we have reached the maximum window count
    if (count_ >= MAX_WINDOWS || win == nullptr) {
        return 0;
    }

    // Place the window at the top of Z-order
    windows_[count_] = win;

    // Draw the title bar and content onto the window's off-screen canvas
    if (font_ != nullptr) {
        windows_[count_]->draw_title_bar(*font_);
    }
    windows_[count_]->draw_content();

    count_++;

    // Update focus to the new top-most window
    update_focus();

    return windows_[count_ - 1]->id();
}

void WindowManager::destroy(uint32_t id) {
    // Search for the window by ID
    uint32_t idx = find_index(id);
    if (idx < MAX_WINDOWS) {
        remove_at(idx);
    }
}

void WindowManager::raise(uint32_t id) {
    uint32_t idx = find_index(id);

    // Not found
    if (idx >= MAX_WINDOWS) {
        return;
    }

    // Already at the top -- still update focus (may be nullptr after desktop click)
    if (idx == count_ - 1) {
        update_focus();
        return;
    }

    // Save the pointer, shift windows above it down, place at top
    Window* win = windows_[idx];

    for (uint32_t i = idx; i < count_ - 1; i++) {
        windows_[i] = windows_[i + 1];
    }

    windows_[count_ - 1] = win;

    // Update focus to the new top-most window
    update_focus();
}

// ============================================================
// Desktop icon management
// ============================================================

Window* WindowManager::window_at(uint32_t index) const {
    if (index >= count_) {
        return nullptr;
    }
    return windows_[index];
}

bool WindowManager::add_desktop_icon(const DesktopIcon& icon) {
    if (icon_count_ >= MAX_ICONS) {
        return false;
    }

    icons_[icon_count_] = icon;
    icon_count_++;
    return true;
}

const DesktopIcon* WindowManager::hit_test_icon(int32_t mx, int32_t my) const {
    // Iterate from last-registered to first so later icons take priority
    for (uint32_t i = icon_count_; i > 0; i--) {
        uint32_t idx = i - 1;
        if (icons_[idx].contains(mx, my)) {
            return &icons_[idx];
        }
    }
    return nullptr;
}

IconAction WindowManager::consume_pending_icon_action() {
    IconAction action    = pending_icon_action_;
    pending_icon_action_ = IconAction::None;
    return action;
}

// ============================================================
// Compositing
// ============================================================

void WindowManager::composite() {
    if (screen_ == nullptr) {
        return;
    }

    // Clear the screen back buffer with the desktop background colour
    screen_->clear(DESKTOP_COLOR);

    // Draw desktop icons behind all windows
    draw_desktop_icons(*screen_);

    // Blit each window from lowest Z-order (index 0) to highest
    for (uint32_t i = 0; i < count_; i++) {
        if (windows_[i]->visible()) {
            windows_[i]->blit_to(*screen_);
        }
    }

    // Draw the mouse cursor on top of everything
    draw_cursor(*screen_);

    // F13 §4c: the frame is NOT presented here. The cinux::gui pump flushes the dirty
    // region to the host (which forwards to the framebuffer) after composite()
    // returns. See pump(). Behaviour is identical to the old flip(); the
    // display path now runs through the Host ABI so it is host-agnostic.
}

// ============================================================
// Dirty region (F13 §4c)
// ============================================================

void WindowManager::invalidate(const cinux::gui::Rect& r) {
    if (screen_ == nullptr || r.empty()) {
        return;
    }
    /* Clip to the screen so we never track off-screen area. */
    cinux::gui::Rect clipped =
        cinux::gui::rect_intersect(r, cinux::gui::Rect{0, 0, static_cast<int32_t>(screen_->width()),
                                             static_cast<int32_t>(screen_->height())});
    if (!clipped.empty()) {
        dirty_.add(clipped);
    }
}

void WindowManager::invalidate_all() {
    if (screen_ == nullptr) {
        return;
    }
    invalidate(cinux::gui::Rect{0, 0, static_cast<int32_t>(screen_->width()),
                           static_cast<int32_t>(screen_->height())});
}

void WindowManager::invalidate_cursor_move() {
    if (screen_ == nullptr) {
        return;
    }
    const int32_t cx = cinux::drivers::Mouse::x();
    const int32_t cy = cinux::drivers::Mouse::y();

    /* First frame: no previous footprint -- flush everything to be safe. */
    if (last_cursor_x_ == kCursorInvalid || last_cursor_y_ == kCursorInvalid) {
        invalidate_all();
        last_cursor_x_ = cx;
        last_cursor_y_ = cy;
        return;
    }

    if (cx != last_cursor_x_ || cy != last_cursor_y_) {
        /* The cursor bitmap is 16x16 with a 1px outline; pad by 2px each side
         * so the old + new footprints (outline included) are fully covered.
         * Over-cover is safe; the back buffer is always fully repainted. */
        const int32_t pad      = 2;
        const int32_t cur_size = static_cast<int32_t>(CURSOR_SIZE);
        invalidate(cinux::gui::Rect{last_cursor_x_ - pad, last_cursor_y_ - pad,
                               last_cursor_x_ + cur_size + pad, last_cursor_y_ + cur_size + pad});
        invalidate(cinux::gui::Rect{cx - pad, cy - pad, cx + cur_size + pad, cy + cur_size + pad});
    }
    last_cursor_x_ = cx;
    last_cursor_y_ = cy;
}

// ============================================================
// Cursor rendering
// ============================================================

void WindowManager::draw_cursor(cinux::drivers::Canvas& screen) {
    int32_t cx = cinux::drivers::Mouse::x();
    int32_t cy = cinux::drivers::Mouse::y();

    for (uint32_t row = 0; row < CURSOR_SIZE; row++) {
        uint16_t bits = k_cursor_bitmap[row];
        for (uint32_t col = 0; col < CURSOR_SIZE; col++) {
            if (bits & (0x8000 >> col)) {
                int32_t px = cx + static_cast<int32_t>(col);
                int32_t py = cy + static_cast<int32_t>(row);

                // Draw black outline for visibility
                screen.draw_pixel(static_cast<uint32_t>(px - 1), static_cast<uint32_t>(py),
                                  CURSOR_BLACK);
                screen.draw_pixel(static_cast<uint32_t>(px + 1), static_cast<uint32_t>(py),
                                  CURSOR_BLACK);
                screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py - 1),
                                  CURSOR_BLACK);
                screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py + 1),
                                  CURSOR_BLACK);

                // Draw white cursor pixel
                screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                                  CURSOR_WHITE);
            }
        }
    }
}

// ============================================================
// Desktop icon rendering
// ============================================================

void WindowManager::draw_desktop_icons(cinux::drivers::Canvas& screen) {
    if (font_ == nullptr) {
        return;
    }

    uint32_t glyph_w = font_->width();

    for (uint32_t i = 0; i < icon_count_; i++) {
        const DesktopIcon& icon = icons_[i];

        // Draw the icon bitmap via its 1-bpp alpha mask (§4d): transparency is
        // mask-driven, not the legacy 0x00000000 colorkey, so opaque pure-black
        // icon pixels are now draw-able.
        screen.draw_bitmap_masked(static_cast<uint32_t>(icon.x), static_cast<uint32_t>(icon.y),
                                  icon.width, icon.height, icon.bitmap, icon.mask);

        // Compute label length
        uint32_t label_len = 0;
        if (icon.label != nullptr) {
            for (const char* p = icon.label; *p != '\0'; p++) {
                label_len++;
            }
        }

        if (label_len > 0) {
            // Centre the label text horizontally below the icon bitmap
            uint32_t text_w  = label_len * glyph_w;
            uint32_t label_x = static_cast<uint32_t>(icon.x) + (icon.width - text_w) / 2;
            uint32_t label_y = static_cast<uint32_t>(icon.y) + icon.height + 2;

            screen.draw_text(label_x, label_y, icon.label, ICON_LABEL_COLOR, *font_);
        }
    }
}

// ============================================================
// Internal helpers
// ============================================================

Window* WindowManager::find_window(uint32_t id) {
    for (uint32_t i = 0; i < count_; i++) {
        if (windows_[i]->id() == id) {
            return windows_[i];
        }
    }
    return nullptr;
}

uint32_t WindowManager::find_index(uint32_t id) const {
    for (uint32_t i = 0; i < count_; i++) {
        if (windows_[i]->id() == id) {
            return i;
        }
    }
    return MAX_WINDOWS;
}

Window* WindowManager::hit_test(int32_t mx, int32_t my) {
    // Iterate from top-most (count_ - 1) down to bottom (0)
    for (uint32_t i = count_; i > 0; i--) {
        uint32_t idx = i - 1;
        if (windows_[idx]->visible() && windows_[idx]->contains(mx, my)) {
            return windows_[idx];
        }
    }
    return nullptr;
}

void WindowManager::remove_at(uint32_t idx) {
    if (idx >= count_) {
        return;
    }

    bool was_focused = (focused_ != nullptr && focused_->id() == windows_[idx]->id());

    // Free the window memory
    delete windows_[idx];
    windows_[idx] = nullptr;

    // Shift windows above idx down by one slot
    for (uint32_t i = idx; i < count_ - 1; i++) {
        windows_[i] = windows_[i + 1];
    }

    // Clear the vacated top slot
    windows_[count_ - 1] = nullptr;
    count_--;

    // If the removed window was focused, update focus
    if (was_focused) {
        update_focus();
    }
}

void WindowManager::update_focus() {
    // Clear focus on all windows
    for (uint32_t i = 0; i < count_; i++) {
        windows_[i]->set_focused(false);
    }

    // Set focus on the top-most window, if any
    if (count_ > 0) {
        focused_ = windows_[count_ - 1];
        focused_->set_focused(true);
    } else {
        focused_ = nullptr;
    }
}

}  // namespace cinux::gui
