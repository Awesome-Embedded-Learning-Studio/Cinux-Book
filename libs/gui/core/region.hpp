/**
 * @file core/region.hpp
 * @brief cgui L3 region algebra -- first-class Rect + bounded Region (F13 §4b)
 *
 * A region is the unit of "what changed" in the render engine. The §4c dirty
 * tracker accumulates a Region per frame and the host flushes exactly those
 * rects; the compositor's occlusion / subtract (§5) and MCU page-band lowering
 * (cgui-02 §4.4) build on the same algebra.
 *
 * Design rules (correctness over cleverness -- this governs what reaches the
 * screen, so a wrong answer is a visible glitch):
 *   - All rects are half-open [x0,x1) x [y0,y1), same layout as ClipRect (§4a).
 *   - A rect with x0>=x1 or y0>=y1 is DEGENERATE (empty): area 0, contains
 *     nothing. Region never stores degenerate rects.
 *   - Region has a hard capacity (kMaxRects). On overflow it COLLAPSES to its
 *     bounding box -- a conservative over-approximation. A region therefore
 *     NEVER under-covers: it may flush pixels that did not change, but it never
 *     drops a pixel that did. For dirty regions, over-coverage is a perf cost;
 *     under-coverage is a stale-pixel bug.
 *
 * Integer-only (integer-only safe), no allocation (fixed storage).
 *
 * Compile condition: CINUX_GUI.
 *
 * Namespace: cgui
 */
#pragma once

#include <stdint.h>

namespace cinux::gui {

/**
 * @brief Half-open 2D rectangle [x0,x1) x [y0,y1)
 *
 * Layout-identical to ClipRect (§4a) so the two interconvert by trivial copy;
 * Rect is the canonical name for region algebra. Degenerate when x0>=x1 or
 * y0>=y1.
 */
struct Rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;

    /** True if the rect covers no area (a coordinate was inverted/empty). */
    constexpr bool empty() const { return x0 >= x1 || y0 >= y1; }

    /** Width in pixels, 0 if empty. */
    constexpr uint32_t width() const { return empty() ? 0u : static_cast<uint32_t>(x1 - x0); }

    /** Height in pixels, 0 if empty. */
    constexpr uint32_t height() const { return empty() ? 0u : static_cast<uint32_t>(y1 - y0); }

    /** True if point (px,py) lies inside the half-open rect. */
    constexpr bool contains(int32_t px, int32_t py) const {
        return !empty() && px >= x0 && px < x1 && py >= y0 && py < y1;
    }

    /** True if @p o lies entirely inside this rect (both must be non-empty). */
    constexpr bool contains(const Rect& o) const {
        return !empty() && !o.empty() && o.x0 >= x0 && o.y0 >= y0 && o.x1 <= x1 && o.y1 <= y1;
    }
};

/** Largest overlap of @p a and @p b; degenerate if they do not overlap. */
constexpr Rect rect_intersect(const Rect& a, const Rect& b) {
    return Rect{(a.x0 > b.x0 ? a.x0 : b.x0), (a.y0 > b.y0 ? a.y0 : b.y0),
                (a.x1 < b.x1 ? a.x1 : b.x1), (a.y1 < b.y1 ? a.y1 : b.y1)};
}

/** Smallest rect enclosing @p a and @p b; degenerate only if both are empty. */
constexpr Rect rect_union(const Rect& a, const Rect& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return Rect{(a.x0 < b.x0 ? a.x0 : b.x0), (a.y0 < b.y0 ? a.y0 : b.y0),
                (a.x1 > b.x1 ? a.x1 : b.x1), (a.y1 > b.y1 ? a.y1 : b.y1)};
}

/** @p a translated by (dx,dy); degenerate stays degenerate. */
constexpr Rect rect_translate(const Rect& a, int32_t dx, int32_t dy) {
    return Rect{a.x0 + dx, a.y0 + dy, a.x1 + dx, a.y1 + dy};
}

/**
 * @brief Subtract @p b from @p a, writing up to 4 non-empty result rects
 *
 * Computes the set difference a \ b as the (up to) four strips of @p a that
 * remain after carving out the intersection with @p b. The strips are: the
 * band above b, the band below b, the strip left of b, and the strip right of
 * b (all clipped to a). Output order is top, bottom, left, right; only the
 * non-empty strips are written.
 *
 * @param a    The rect to subtract from
 * @param b    The rect to remove
 * @param out  Output buffer with room for 4 rects
 * @return     Number of result rects written (0 if a is covered/empty, up to 4)
 */
uint32_t rect_subtract(const Rect& a, const Rect& b, Rect out[4]);

/**
 * @brief Fixed-capacity region of rects with bounding-box collapse on overflow
 *
 * Members are not guaranteed disjoint (add does not split against existing
 * rects); the region is a union-of-rects approximation. Operations that would
 * exceed kMaxRects (subtract can multiply the count) collapse the whole region
 * to its bounding box so the region stays bounded and never under-covers.
 */
class Region {
public:
    /** Hard cap on stored rects before collapsing to the bounding box. */
    static constexpr uint32_t kMaxRects = 32;

    /** Construct an empty region. */
    Region() = default;

    /** Remove all rects. */
    void clear() { count_ = 0; }

    /**
     * @brief Add a rect to the region
     *
     * Degenerate rects are ignored. On capacity overflow the whole region
     * collapses to its bounding box (the envelope of all current members plus
     * @p r), preserving coverage.
     */
    void add(const Rect& r);

    /**
     * @brief Clip every member to @p clip, dropping those that fall outside
     */
    void intersect(const Rect& clip);

    /** Shift every member by (dx,dy). */
    void translate(int32_t dx, int32_t dy);

    /**
     * @brief Subtract @p r from every member
     *
     * Each member is split via rect_subtract; the fragments replace it. On
     * capacity overflow the region collapses to its bounding box.
     */
    void subtract(const Rect& r);

    /** True if no rects remain. */
    bool empty() const { return count_ == 0; }

    /** Number of stored rects. */
    uint32_t count() const { return count_; }

    /** Read-only access to the stored rect array. */
    const Rect* rects() const { return rects_; }

    /** Smallest rect enclosing every member; degenerate if empty. */
    Rect bounds() const;

private:
    Rect     rects_[kMaxRects];
    uint32_t count_ = 0;
};

}  // namespace cinux::gui
