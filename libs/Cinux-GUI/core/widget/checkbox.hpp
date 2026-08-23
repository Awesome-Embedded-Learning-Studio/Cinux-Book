/**
 * @file core/widget/checkbox.hpp
 * @brief CheckBox -- click-to-toggle box (P6-a)
 *
 * A press toggles `checked`; paints a hollow outline box (16px) that fills with
 * primary when checked. The box is vertically centred in the widget rect so an
 * HBox can pair it with a Label.
 *
 * Pure C++17 (stdint/stddef + core/ only), ZERO host includes.
 * Compile condition: CINUX_GUI.
 */
#pragma once

#include <stdint.h>

#include "../event_payload.hpp"  // PointerPayload
#include "../paint_list.hpp"     // PaintList
#include "../theme.hpp"          // Theme
#include "../widget.hpp"         // Widget

namespace cinux::gui {

class CheckBox : public Widget {
public:
    void set_theme(const Theme* th) { theme_ = th; }
    bool checked() const { return checked_; }
    void on_pointer(const PointerPayload& p) override;

protected:
    void paint_to_list(PaintList& list) const override;

private:
    bool         checked_ = false;
    const Theme* theme_   = nullptr;
};

}  // namespace cinux::gui
