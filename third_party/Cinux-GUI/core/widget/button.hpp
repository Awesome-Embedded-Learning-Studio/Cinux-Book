/**
 * @file core/widget/button.hpp
 * @brief Button widget -- Material contained button with press state (P3-c)
 *
 * Rest: primary-coloured rounded fill + on_primary text. Pressed (pointer down
 * inside): surface fill + primary text -- a clear visual toggle without blur or
 * ripple (P3 decision B: flat Material). Reads colours/radius from a Theme.
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

class Button : public Widget {
public:
    void set_text(const char* t) { text_ = t; }
    void set_theme(const Theme* th) { theme_ = th; }
    bool pressed() const { return pressed_; }

protected:
    void paint_to_list(PaintList& list) const override;
    void on_pointer(const PointerPayload& p) override;

private:
    const char*  text_    = "";
    const Theme* theme_   = nullptr;
    bool         pressed_ = false;
};

}  // namespace cinux::gui
