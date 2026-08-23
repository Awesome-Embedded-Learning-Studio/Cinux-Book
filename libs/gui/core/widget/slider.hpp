/**
 * @file core/widget/slider.hpp
 * @brief Slider widget -- draggable value thumb (P3-d)
 *
 * A horizontal track with a round thumb whose position encodes value in
 * [0, range]. Uses Desktop press capture (P3-d): the down captures the slider
 * so move events keep arriving during a drag even off the thumb. Paints the
 * track (outline) + thumb (primary) from the Theme.
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

class Slider : public Widget {
public:
    void    set_theme(const Theme* th) { theme_ = th; }
    void    set_range(int32_t r) { range_ = r; }
    void    set_value(int32_t v);
    int32_t value() const { return value_; }

protected:
    void paint_to_list(PaintList& list) const override;
    void on_pointer(const PointerPayload& p) override;

private:
    void apply_x_(int32_t x);  // map a pointer x -> clamped value

    const Theme* theme_    = nullptr;
    int32_t      value_    = 0;
    int32_t      range_    = 100;
    bool         dragging_ = false;
};

}  // namespace cinux::gui
