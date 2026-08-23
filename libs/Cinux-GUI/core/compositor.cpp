/**
 * @file core/compositor.cpp
 * @brief cinux::gui Compositor -- PaintList -> staging pixels via swraster
 *
 * render() dispatches each cmd to a registered handler (Compositor::handlers_).
 * New primitives register via set_handler(); render() never grows a bigger
 * switch. P2's Scene path is gone (P6-d).
 *
 * Host-neutral, ZERO host includes.
 *
 * Compile condition: CINUX_GUI.
 */

#include "compositor.hpp"

#include <stdint.h>

#include "swraster.hpp"  // ClipRect, fill_rect, fill_rounded_rect, glyph_blit(_scaled)

namespace cinux::gui {
namespace {

/* Render a NUL-terminated string with glyph_blit; '\n' wraps a row. */
void draw_text(Surface& s, const PsfFont& font, const char* str, int32_t x, int32_t y,
               uint32_t color, const ClipRect* clip) {
    if (str == nullptr) {
        return;
    }
    int32_t cx = x;
    int32_t cy = y;
    for (const char* p = str; *p != '\0'; p++) {
        if (*p == '\n') {
            cx = x;
            cy += static_cast<int32_t>(font.height());
            continue;
        }
        glyph_blit(s, cx, cy, font.glyph(static_cast<uint8_t>(*p)), font.width(), font.height(),
                   color, clip);
        cx += static_cast<int32_t>(font.width());
    }
}

/* Like draw_text but each glyph is glyph_blit_scaled at @p scale (P5-a). */
void draw_text_scaled(Surface& s, const PsfFont& font, const char* str, int32_t x, int32_t y,
                      uint32_t color, uint32_t scale, const ClipRect* clip) {
    if (str == nullptr) {
        return;
    }
    int32_t cx = x;
    int32_t cy = y;
    for (const char* p = str; *p != '\0'; p++) {
        if (*p == '\n') {
            cx = x;
            cy += static_cast<int32_t>(font.height() * scale);
            continue;
        }
        glyph_blit_scaled(s, cx, cy, font.glyph(static_cast<uint8_t>(*p)), font.width(),
                          font.height(), scale, color, clip);
        cx += static_cast<int32_t>(font.width() * scale);
    }
}

/* ---- default primitive handlers (one per drawable CmdKind) ----
 * Each pulls its payload out of the PaintCmd union and calls the matching
 * swraster primitive. Adding a shape later = a new one of these + set_handler. */

void h_fill_rect(Surface& s, const PaintCmd& c, const PsfFont& /*font*/, const ClipRect* clip) {
    fill_rect(s, c.fill.x, c.fill.y, c.fill.w, c.fill.h, c.fill.color, clip);
}

void h_fill_round(Surface& s, const PaintCmd& c, const PsfFont& /*font*/, const ClipRect* clip) {
    fill_rounded_rect(s, c.rfill.x, c.rfill.y, c.rfill.w, c.rfill.h, c.rfill.color, c.rfill.radius,
                      clip);
}

void h_text(Surface& s, const PaintCmd& c, const PsfFont& font, const ClipRect* clip) {
    draw_text(s, font, c.text.text, c.text.x, c.text.y, c.text.color, clip);
}

void h_text_glyph(Surface& s, const PaintCmd& c, const PsfFont& font, const ClipRect* clip) {
    const uint8_t* bits = font.glyph(static_cast<uint8_t>(c.glyph.ch));
    if (bits != nullptr) {
        glyph_blit(s, c.glyph.x, c.glyph.y, bits, font.width(), font.height(), c.glyph.color, clip);
    }
}

void h_text_scaled(Surface& s, const PaintCmd& c, const PsfFont& font, const ClipRect* clip) {
    draw_text_scaled(s, font, c.scaled.text, c.scaled.x, c.scaled.y, c.scaled.color, c.scaled.scale,
                     clip);
}

}  // namespace

Compositor::Compositor() {
    handlers_[static_cast<uint32_t>(CmdKind::kFillRect)]      = h_fill_rect;
    handlers_[static_cast<uint32_t>(CmdKind::kFillRoundRect)] = h_fill_round;
    handlers_[static_cast<uint32_t>(CmdKind::kText)]          = h_text;
    handlers_[static_cast<uint32_t>(CmdKind::kTextGlyph)]     = h_text_glyph;
    handlers_[static_cast<uint32_t>(CmdKind::kTextScaled)]    = h_text_scaled;
    // kClipPush / kClipPop are handled inside render() (they drive the clip stack).
}

void Compositor::set_handler(CmdKind kind, Handler h) {
    const uint32_t k = static_cast<uint32_t>(kind);
    if (k < kKindCount) {
        handlers_[k] = h;
    }
}

void Compositor::set_cursor(int32_t x, int32_t y, bool visible) {  // P7-c
    cursor_x_       = x;
    cursor_y_       = y;
    cursor_visible_ = visible;
}

void Compositor::render(Surface& staging, const PaintList& list, const PsfFont& font,
                        const ClipRect* outer) {
    /* Clip stack: each kClipPush intersects with its parent (and the optional
     * @p outer base), so a widget can never paint outside its ancestors' rects.
     * top == -1 means unclipped (primitives clip to surface bounds via nullptr). */
    constexpr uint32_t kMaxClip = 32;
    ClipRect           stack[kMaxClip];
    int32_t            top = -1;
    if (outer != nullptr) {
        stack[++top] = *outer;  // P5-f: base clip (dirty rect)
    }
    const auto cur = [&]() -> const ClipRect* { return top >= 0 ? &stack[top] : nullptr; };

    const uint32_t n = list.count();
    for (uint32_t i = 0u; i < n; i++) {
        const PaintCmd& c = list.at(i);
        if (c.kind == CmdKind::kClipPush) {
            if (top + 1 < static_cast<int32_t>(kMaxClip)) {
                ClipRect pushed{c.clip.x0, c.clip.y0, c.clip.x1, c.clip.y1};
                if (top >= 0) {
                    const ClipRect& p = stack[top];
                    pushed            = ClipRect{
                        pushed.x0 > p.x0 ? pushed.x0 : p.x0, pushed.y0 > p.y0 ? pushed.y0 : p.y0,
                        pushed.x1 < p.x1 ? pushed.x1 : p.x1, pushed.y1 < p.y1 ? pushed.y1 : p.y1};
                }
                stack[++top] = pushed;
            }
            continue;
        }
        if (c.kind == CmdKind::kClipPop) {
            if (top >= 0) {
                top--;
            }
            continue;
        }
        const uint32_t k = static_cast<uint32_t>(c.kind);
        if (k < kKindCount && handlers_[k] != nullptr) {
            handlers_[k](staging, c, font, cur());
        }
    }

    /* Cursor on top of everything: the legacy CinuxOS 16x16 arrow bitmap
     * (1-bpp, MSB-first per row), each lit pixel painted as a 1px grey body
     * with a 1px white outline for visibility on the dark theme. The 4x4
     * block this replaced read as a faint blob. Clipped to cur() so under
     * per-rect render it only paints inside the dirty rect that contains the
     * cursor (no double-paint across rects). */
    if (cursor_visible_) {
        static constexpr uint16_t kCursorBitmap[16] = {
            0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
            0xFF00, 0xF800, 0xE000, 0xC000, 0x8800, 0x0800, 0x0000, 0x0000,
        };
        constexpr uint32_t kCursorBody    = 0x00888888u; /* grey fill (legacy "white") */
        constexpr uint32_t kCursorOutline = 0x00FFFFFFu; /* white edge (legacy "black") */
        for (uint32_t row = 0; row < 16u; ++row) {
            const uint16_t bits = kCursorBitmap[row];
            for (uint32_t col = 0; col < 16u; ++col) {
                if ((bits & (0x8000u >> col)) == 0u) {
                    continue;
                }
                const int32_t px = cursor_x_ + static_cast<int32_t>(col);
                const int32_t py = cursor_y_ + static_cast<int32_t>(row);
                fill_rect(staging, px - 1, py, 1u, 1u, kCursorOutline, cur());
                fill_rect(staging, px + 1, py, 1u, 1u, kCursorOutline, cur());
                fill_rect(staging, px, py - 1, 1u, 1u, kCursorOutline, cur());
                fill_rect(staging, px, py + 1, 1u, 1u, kCursorOutline, cur());
                fill_rect(staging, px, py, 1u, 1u, kCursorBody, cur());
            }
        }
    }
}

}  // namespace cinux::gui
