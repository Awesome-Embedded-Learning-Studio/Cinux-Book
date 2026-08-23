/**
 * @file core/paint_list.cpp
 * @brief cinux::gui PaintList -- fixed-capacity cmd buffer push helpers
 *
 * Each push drops the cmd on overflow (count_ stays at kMaxCmds) -- core never
 * aborts. The widget tree owns the lifetime of any borrowed text pointer; it
 * outlives execute() within the same frame.
 *
 * Compile condition: CINUX_GUI.
 */

#include "paint_list.hpp"

#include <stdint.h>

namespace cinux::gui {

void PaintList::fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (count_ >= kMaxCmds) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kFillRect;
    c.fill      = {x, y, w, h, color};
}

void PaintList::fill_round_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
                                uint32_t radius) {
    fill_round_rect_corners(x, y, w, h, color, radius, 0xFu);  // all corners (P5-d)
}

void PaintList::fill_round_rect_corners(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                        uint32_t color, uint32_t radius, uint32_t corners) {
    if (count_ >= kMaxCmds) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kFillRoundRect;
    c.rfill     = {x, y, w, h, color, radius, corners};
}

void PaintList::text(int32_t x, int32_t y, uint32_t color, const char* str) {
    if (count_ >= kMaxCmds || str == nullptr) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kText;
    c.text      = {x, y, color, str};
}

void PaintList::text_glyph(int32_t x, int32_t y, uint32_t color, char ch) {
    if (count_ >= kMaxCmds) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kTextGlyph;
    c.glyph     = {x, y, color, ch};
}

void PaintList::text_scaled(int32_t x, int32_t y, uint32_t color, const char* str, uint32_t scale) {
    if (count_ >= kMaxCmds || str == nullptr) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kTextScaled;
    c.scaled    = {x, y, color, scale, str};
}

void PaintList::clip_push(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (count_ >= kMaxCmds) {
        return;
    }
    PaintCmd& c = cmds_[count_++];
    c.kind      = CmdKind::kClipPush;
    c.clip      = {x0, y0, x1, y1};
}

void PaintList::clip_pop() {
    if (count_ >= kMaxCmds) {
        return;
    }
    cmds_[count_++].kind = CmdKind::kClipPop;
}

}  // namespace cinux::gui
