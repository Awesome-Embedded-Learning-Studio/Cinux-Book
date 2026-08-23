/**
 * @file core/swraster.cpp
 * @brief cgui L3 software rasteriser primitive implementations (§4a skeleton)
 *
 * See swraster.hpp for scope. All primitives are integer-only (Q8.8
 * blend) and clip to clip ∩ surface bounds. §4a implements XRGB8888 only;
 * other formats are no-ops here and gain their paths with profile support.
 *
 * §4a: NOT wired into pump yet -- pump still composites via
 * wm.composite(). The SwRaster takes over rendering in §4c (staging buffer +
 * dirty region + flush). These primitives are exercised by unit tests only
 * until then.
 *
 * Compile condition: CINUX_GUI.
 */

#include "swraster.hpp"

#include <stdint.h>

namespace cinux::gui {
namespace {

/* Intersect [a0,a1) with [b0,b1); write the overlap to [o0,o1).
 * Returns false if the intersection is empty (o0 >= o1). */
bool isect(int32_t a0, int32_t a1, int32_t b0, int32_t b1, int32_t& o0, int32_t& o1) {
    o0 = a0 > b0 ? a0 : b0;
    o1 = a1 < b1 ? a1 : b1;
    return o0 < o1;
}

/* Resolve the effective bounds = clip (if any) intersected with surface bounds.
 * A clip rect that exceeds the surface is clamped down to it. */
inline void effective_bounds(const Surface& s, const ClipRect* clip, int32_t& ex0, int32_t& ey0,
                             int32_t& ex1, int32_t& ey1) {
    ex0 = 0;
    ey0 = 0;
    ex1 = static_cast<int32_t>(s.width);
    ey1 = static_cast<int32_t>(s.height);
    if (clip != nullptr) {
        if (clip->x0 > ex0) ex0 = clip->x0;
        if (clip->y0 > ey0) ey0 = clip->y0;
        if (clip->x1 < ex1) ex1 = clip->x1;
        if (clip->y1 < ey1) ey1 = clip->y1;
    }
}

inline uint32_t* xrgb_row(Surface& s, int32_t y) {
    const uint32_t ppr = s.stride_bytes / 4u;
    return static_cast<uint32_t*>(s.pixels) + static_cast<uint32_t>(y) * ppr;
}

inline const uint32_t* xrgb_crow(const Surface& s, int32_t y) {
    const uint32_t ppr = s.stride_bytes / 4u;
    return static_cast<const uint32_t*>(s.pixels) + static_cast<uint32_t>(y) * ppr;
}

/* Integer square root (floor). Used by fill_rounded_rect's per-row corner
 * offset; inputs stay small (<= radius², radius<=16 -> <=256), so a linear
 * scan is fine and stays integer-only. */
uint32_t isqrt_u32(uint32_t n) {
    uint32_t k = 0u;
    while ((k + 1u) * (k + 1u) <= n) {
        k++;
    }
    return k;
}

}  // namespace

void fill_rect(Surface& s, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
               const ClipRect* clip) {
    if (s.format != PixelFormat::kXrgb8888) {
        return; /* §4a: XRGB8888 only */
    }
    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(s, clip, ex0, ey0, ex1, ey1);
    int32_t rx0, rx1, ry0, ry1;
    if (!isect(x, x + static_cast<int32_t>(w), ex0, ex1, rx0, rx1)) {
        return;
    }
    if (!isect(y, y + static_cast<int32_t>(h), ey0, ey1, ry0, ry1)) {
        return;
    }
    for (int32_t r = ry0; r < ry1; r++) {
        uint32_t* row = xrgb_row(s, r);
        for (int32_t c = rx0; c < rx1; c++) {
            row[c] = color;
        }
    }
}

void fill_rounded_rect(Surface& s, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color,
                       uint32_t radius, const ClipRect* clip, uint32_t corners) {
    if (s.format != PixelFormat::kXrgb8888 || w == 0u || h == 0u) {
        return;
    }
    /* Clamp radius to min(w,h)/2 so corner arcs never overlap. */
    uint32_t r      = radius;
    uint32_t half_w = w / 2u;
    uint32_t half_h = h / 2u;
    if (r > half_w) {
        r = half_w;
    }
    if (r > half_h) {
        r = half_h;
    }
    if (r == 0u || corners == 0u) {
        fill_rect(s, x, y, w, h, color, clip);
        return;
    }

    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(s, clip, ex0, ey0, ex1, ey1);
    const int32_t x1 = x + static_cast<int32_t>(w);
    const int32_t y1 = y + static_cast<int32_t>(h);
    int32_t       rx0, rx1, ry0, ry1;
    if (!isect(x, x1, ex0, ex1, rx0, rx1)) {
        return;
    }
    if (!isect(y, y1, ey0, ey1, ry0, ry1)) {
        return;
    }

    const uint32_t rr  = r * r;
    const uint32_t ppr = s.stride_bytes / 4u;
    uint32_t*      buf = static_cast<uint32_t*>(s.pixels);
    const int32_t  ir  = static_cast<int32_t>(r);
    const int32_t  ih  = static_cast<int32_t>(h);

    /* P5-d: per-corner. Each row computes how far the arc bites in from the
     * left/right edge, considering only the enabled corners near the top and
     * bottom edges. A square corner bites in 0. */
    for (int32_t py = ry0; py < ry1; py++) {
        const int32_t iy    = py - y;      /* 0..h-1 */
        const int32_t top_d = iy;          /* rows from the top edge */
        const int32_t bot_d = ih - 1 - iy; /* rows from the bottom edge */
        int32_t       off_l = 0;
        int32_t       off_r = 0;
        if (top_d < ir && (corners & (kCornerTL | kCornerTR)) != 0u) {
            const int32_t  d      = ir - 1 - top_d;
            const uint32_t inside = (d >= 0) ? rr - static_cast<uint32_t>(d) * d : rr;
            const int32_t  off    = ir - static_cast<int32_t>(isqrt_u32(inside));
            if ((corners & kCornerTL) != 0u && off > off_l) {
                off_l = off;
            }
            if ((corners & kCornerTR) != 0u && off > off_r) {
                off_r = off;
            }
        }
        if (bot_d < ir && (corners & (kCornerBL | kCornerBR)) != 0u) {
            const int32_t  d      = ir - 1 - bot_d;
            const uint32_t inside = (d >= 0) ? rr - static_cast<uint32_t>(d) * d : rr;
            const int32_t  off    = ir - static_cast<int32_t>(isqrt_u32(inside));
            if ((corners & kCornerBL) != 0u && off > off_l) {
                off_l = off;
            }
            if ((corners & kCornerBR) != 0u && off > off_r) {
                off_r = off;
            }
        }
        int32_t sx0 = x + off_l;
        int32_t sx1 = x1 - off_r;
        if (sx0 < rx0) {
            sx0 = rx0;
        }
        if (sx1 > rx1) {
            sx1 = rx1;
        }
        if (sx0 >= sx1) {
            continue;
        }
        uint32_t* drow = buf + static_cast<uint32_t>(py) * ppr;
        for (int32_t c = sx0; c < sx1; c++) {
            drow[c] = color;
        }
    }
}

void blit(Surface& dst, int32_t dx, int32_t dy, const Surface& src, uint32_t sx, uint32_t sy,
          uint32_t w, uint32_t h, const ClipRect* clip) {
    if (dst.format != PixelFormat::kXrgb8888 || src.format != PixelFormat::kXrgb8888) {
        return;
    }
    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(dst, clip, ex0, ey0, ex1, ey1);
    int32_t rx0, rx1, ry0, ry1;
    if (!isect(dx, dx + static_cast<int32_t>(w), ex0, ex1, rx0, rx1)) {
        return;
    }
    if (!isect(dy, dy + static_cast<int32_t>(h), ey0, ey1, ry0, ry1)) {
        return;
    }
    /* Map destination clip back to source: src_x = sx + (dst_x - dx). */
    for (int32_t r = ry0; r < ry1; r++) {
        const uint32_t src_r = sy + static_cast<uint32_t>(r - dy);
        if (src_r >= src.height) {
            break;
        }
        uint32_t*       drow = xrgb_row(dst, r);
        const uint32_t* srow = xrgb_crow(src, static_cast<int32_t>(src_r));
        for (int32_t c = rx0; c < rx1; c++) {
            const uint32_t src_c = sx + static_cast<uint32_t>(c - dx);
            if (src_c >= src.width) {
                break;
            }
            drow[c] = srow[src_c];
        }
    }
}

void blit_blend(Surface& dst, int32_t dx, int32_t dy, const Surface& src, uint32_t sx, uint32_t sy,
                uint32_t w, uint32_t h, uint16_t alpha_q8, const ClipRect* clip) {
    if (dst.format != PixelFormat::kXrgb8888 || src.format != PixelFormat::kXrgb8888) {
        return;
    }
    /* alpha in [0,256]; clamp the upper end (256 = fully opaque, beyond is ill-formed). */
    uint32_t a = alpha_q8;
    if (a > 256u) {
        a = 256u;
    }
    const uint32_t ia = 256u - a;

    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(dst, clip, ex0, ey0, ex1, ey1);
    int32_t rx0, rx1, ry0, ry1;
    if (!isect(dx, dx + static_cast<int32_t>(w), ex0, ex1, rx0, rx1)) {
        return;
    }
    if (!isect(dy, dy + static_cast<int32_t>(h), ey0, ey1, ry0, ry1)) {
        return;
    }
    for (int32_t r = ry0; r < ry1; r++) {
        const uint32_t src_r = sy + static_cast<uint32_t>(r - dy);
        if (src_r >= src.height) {
            break;
        }
        uint32_t*       drow = xrgb_row(dst, r);
        const uint32_t* srow = xrgb_crow(src, static_cast<int32_t>(src_r));
        for (int32_t c = rx0; c < rx1; c++) {
            const uint32_t src_c = sx + static_cast<uint32_t>(c - dx);
            if (src_c >= src.width) {
                break;
            }
            const uint32_t sp = srow[src_c];
            if (a == 256u) {
                drow[c] = sp;
                continue;
            }
            if (a == 0u) {
                continue;
            }
            const uint32_t dp  = drow[c];
            const uint32_t sr  = (sp >> 16) & 0xFFu;
            const uint32_t sg  = (sp >> 8) & 0xFFu;
            const uint32_t sb  = sp & 0xFFu;
            const uint32_t dr  = (dp >> 16) & 0xFFu;
            const uint32_t dg  = (dp >> 8) & 0xFFu;
            const uint32_t db  = dp & 0xFFu;
            const uint32_t or_ = (sr * a + dr * ia) >> 8;
            const uint32_t og  = (sg * a + dg * ia) >> 8;
            const uint32_t ob  = (sb * a + db * ia) >> 8;
            drow[c]            = (or_ << 16) | (og << 8) | ob; /* XRGB: alpha byte 0 */
        }
    }
}

void glyph_blit(Surface& s, int32_t x, int32_t y, const uint8_t* bits, uint32_t gw, uint32_t gh,
                uint32_t color, const ClipRect* clip) {
    if (s.format != PixelFormat::kXrgb8888 || bits == nullptr) {
        return;
    }
    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(s, clip, ex0, ey0, ex1, ey1);
    const uint32_t bytes_per_row = (gw + 7u) / 8u;
    const uint32_t ppr           = s.stride_bytes / 4u;
    uint32_t*      buf           = static_cast<uint32_t*>(s.pixels);
    for (uint32_t row = 0; row < gh; row++) {
        const int32_t dy = y + static_cast<int32_t>(row);
        if (dy < ey0 || dy >= ey1) {
            continue;
        }
        uint32_t* drow = buf + static_cast<uint32_t>(dy) * ppr;
        for (uint32_t col = 0; col < gw; col++) {
            const int32_t dx = x + static_cast<int32_t>(col);
            if (dx < ex0 || dx >= ex1) {
                continue;
            }
            const uint8_t byte = bits[row * bytes_per_row + col / 8u];
            if ((byte >> (7u - (col % 8u))) & 1u) {
                drow[dx] = color;
            }
        }
    }
}

void glyph_blit_scaled(Surface& s, int32_t x, int32_t y, const uint8_t* bits, uint32_t gw,
                       uint32_t gh, uint32_t scale, uint32_t color, const ClipRect* clip) {
    if (s.format != PixelFormat::kXrgb8888 || bits == nullptr || scale == 0u) {
        return;
    }
    /* Same bit layout as glyph_blit (MSB-first, (gw+7)/8 bytes per row); each
     * set bit paints a scale×scale block via fill_rect (which clips). */
    const uint32_t bytes_per_row = (gw + 7u) / 8u;
    for (uint32_t row = 0u; row < gh; ++row) {
        for (uint32_t col = 0u; col < gw; ++col) {
            const uint8_t byte = bits[row * bytes_per_row + col / 8u];
            if (((byte >> (7u - (col % 8u))) & 1u) != 0u) {
                fill_rect(s, x + static_cast<int32_t>(col * scale),
                          y + static_cast<int32_t>(row * scale), scale, scale, color, clip);
            }
        }
    }
}

void draw_line(Surface& s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color,
               const ClipRect* clip) {
    if (s.format != PixelFormat::kXrgb8888) {
        return;
    }
    int32_t ex0, ey0, ex1, ey1;
    effective_bounds(s, clip, ex0, ey0, ex1, ey1);
    const uint32_t ppr = s.stride_bytes / 4u;
    uint32_t*      buf = static_cast<uint32_t*>(s.pixels);

    /* Bresenham over all octants, per-point clipped. */
    int32_t       dx = x1 - x0;
    int32_t       dy = y1 - y0;
    const int32_t sx = dx >= 0 ? 1 : -1;
    const int32_t sy = dy >= 0 ? 1 : -1;
    dx               = dx >= 0 ? dx : -dx;
    dy               = dy >= 0 ? dy : -dy;
    int32_t err      = dx - dy;
    int32_t cx       = x0;
    int32_t cy       = y0;
    while (true) {
        if (cx >= ex0 && cx < ex1 && cy >= ey0 && cy < ey1) {
            buf[static_cast<uint32_t>(cy) * ppr + static_cast<uint32_t>(cx)] = color;
        }
        if (cx == x1 && cy == y1) {
            break;
        }
        const int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx += sx;
        }
        if (e2 < dx) {
            err += dx;
            cy += sy;
        }
    }
}

}  // namespace cinux::gui
