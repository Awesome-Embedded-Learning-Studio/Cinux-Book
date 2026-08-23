/**
 * @file core/swraster.hpp
 * @brief cgui L3 software rasteriser -- pure-CPU pixel primitives (§4a skeleton)
 *
 * The render engine cgui core uses to draw into a staging Surface before
 * flushing it to the host. Everything is integer-only (Q8.8 blend) so it runs
 * on a integer-only profile (MCU-F1) unchanged; no floats leak.
 *
 * §4a scope (see document/todo/f13-gui/cgui-02-refactor-and-separation.md §4):
 * this is the shape skeleton + unit tests. The primitives are generalised from
 * the legacy Canvas draw_* (canvas.cpp) but operate on a bare Surface descriptor
 * (no Canvas dependency) so cgui core stays host-agnostic. They are NOT wired
 * into pump yet -- pump still composites via wm.composite(); the
 * SwRaster takes over rendering in §4c (staging buffer + dirty region + flush).
 *
 * Pixel format: §4a implements XRGB8888 only (Desktop). Other formats
 * (ARGB8888 / RGB565 / 1BPP) assert-fail -- a format mismatch is an ABI
 * violation, not a silent fallback. They arrive with their profile support.
 *
 * Compile condition: CINUX_GUI.
 *
 * Namespace: cgui
 */
#pragma once

#include <stdint.h>

#include "host.hpp"  // PixelFormat

namespace cinux::gui {

/**
 * @brief Half-open clip rectangle [x0,x1) x [y0,y1)
 *
 * Passed to every primitive; the operation is clipped to clip ∩ surface
 * bounds. nullptr means "clip to surface bounds only".
 */
struct ClipRect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

/**
 * @brief Bare pixel-buffer descriptor -- SwRaster does NOT own the buffer
 *
 * The caller (pump staging buffer in §4c, or a host adapter / unit test)
 * owns the storage. stride_bytes is explicit (may exceed width*bpp/8 when the
 * backend aligns rows to a cache line -- presets §4 pixel-format hard contract).
 */
struct Surface {
    void*       pixels;
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride_bytes;
    PixelFormat format;
};

/**
 * @brief Solid rectangle fill (generalises Canvas::draw_rect)
 *
 * Pixels in [x,x+w) x [y,y+h) ∩ clip ∩ surface bounds are set to @p color.
 * Out-of-range regions are silently skipped.
 */
void fill_rect(Surface& s, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
               const ClipRect* clip);

/* Per-corner selection for fill_rounded_rect (P5-d). Bitmask; kCornerAll = all
 * four rounded. A corner bit cleared leaves that corner square. */
inline constexpr uint32_t kCornerTL  = 1u;  // top-left
inline constexpr uint32_t kCornerTR  = 2u;  // top-right
inline constexpr uint32_t kCornerBL  = 4u;  // bottom-left
inline constexpr uint32_t kCornerBR  = 8u;  // bottom-right
inline constexpr uint32_t kCornerAll = 0xFu;

/**
 * @brief Solid rounded-rectangle fill (Material shape; P3-b, per-corner P5-d)
 *
 * Like fill_rect but with @p radius-pixel rounded corners on the corners
 * selected by @p corners (kCorner* bitmask; kCornerAll = all four). radius is
 * clamped to min(w,h)/2; radius == 0 or corners == 0 is exactly fill_rect.
 * Integer-only: per-row corner offset via integer isqrt (floor(sqrt(r²-d²))).
 * XRGB8888 only; other formats no-op.
 */
void fill_rounded_rect(Surface& s, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
                       uint32_t radius, const ClipRect* clip, uint32_t corners = kCornerAll);

/**
 * @brief Opaque pixel copy src -> dst (generalises Canvas::blit)
 *
 * Copies the w x h region from (@p sx,@p sy) of @p src to (@p dx,@p dy) of
 * @p dst, clipped to dst bounds ∩ clip. Source columns/rows that would land
 * outside the clipped destination are skipped (the source offset tracks the
 * destination clip).
 */
void blit(Surface& dst, int32_t dx, int32_t dy, const Surface& src, uint32_t sx, uint32_t sy,
          uint32_t w, uint32_t h, const ClipRect* clip);

/**
 * @brief Q8.8 alpha blend src -> dst (integer-only, integer-only safe)
 *
 * @p alpha_q8 in [0,256]: 0 = dst unchanged, 256 = fully src, 128 = 50/50.
 * Per channel: dst = (src*a + dst*(256-a)) >> 8. XRGB8888 only (alpha byte
 * ignored on input, written 0 on output).
 */
void blit_blend(Surface& dst, int32_t dx, int32_t dy, const Surface& src, uint32_t sx, uint32_t sy,
                uint32_t w, uint32_t h, uint16_t alpha_q8, const ClipRect* clip);

/**
 * @brief 1-bpp alpha-mask -> solid colour blit (glyph rendering)
 *
 * @p bits is a gh-row bitmap, MSB-first within each row, bytes_per_row =
 * (gw+7)/8 bytes per row. For each set bit the corresponding destination
 * pixel is set to @p color; clear bits are skipped (transparent). Generalises
 * the per-glyph loop in Canvas::draw_text.
 */
void glyph_blit(Surface& s, int32_t x, int32_t y, const uint8_t* bits, uint32_t gw, uint32_t gh,
                uint32_t color, const ClipRect* clip);

/**
 * @brief Scaled 1-bpp alpha-mask -> solid colour blit (P5-a: integer font scale)
 *
 * Like glyph_blit but each set bit becomes a scale×scale block (nearest
 * neighbour, via fill_rect). scale==1 reproduces glyph_blit. Lets the bundled
 * 8x16 PSF render as 16x32 / 24x48 for larger UI text without new font data.
 */
void glyph_blit_scaled(Surface& s, int32_t x, int32_t y, const uint8_t* bits, uint32_t gw,
                       uint32_t gh, uint32_t scale, uint32_t color, const ClipRect* clip);

/**
 * @brief Bresenham line (generalises Canvas::draw_line), per-point clipped
 */
void draw_line(Surface& s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color,
               const ClipRect* clip);

}  // namespace cinux::gui
