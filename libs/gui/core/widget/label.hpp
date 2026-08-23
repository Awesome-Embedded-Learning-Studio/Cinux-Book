/**
 * @file core/widget/label.hpp
 * @brief Label widget -- non-interactive text (P3-c)
 *
 * Paints an optional solid background plus a left/top-padded text line. No
 * pointer handling. Pure data over Widget; reads its colours verbatim (callers
 * pass Theme values or custom colours).
 *
 * Compile condition: CINUX_GUI.
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "../paint_list.hpp"  // PaintList
#include "../widget.hpp"      // Widget

namespace cinux::gui {

class Label : public Widget {
public:
    void set_text(const char* t) { text_ = t; }
    void set_color(uint32_t c) { color_ = c; }
    /** Background fill colour; 0 = transparent (no fill, inherits parent). */
    void set_bg(uint32_t bg) { bg_ = bg; }
    /** Integer text scale (1 = native 8x16, 2 = 16x32, ...). P5-a. */
    void set_scale(uint32_t s) { scale_ = s; }

protected:
    void paint_to_list(PaintList& list) const override;

private:
    const char* text_  = "";
    uint32_t    color_ = 0x00FFFFFFu;
    uint32_t    bg_    = 0u;
    uint32_t    scale_ = 1u;
};

}  // namespace cinux::gui
