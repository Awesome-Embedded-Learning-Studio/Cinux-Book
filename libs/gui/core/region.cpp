/**
 * @file core/region.cpp
 * @brief cgui region algebra implementation (F13 §4b)
 *
 * See region.hpp for the design rules (half-open rects, degenerate =
 * empty, capacity collapse to bounding box = never under-cover). Integer-only,
 * fixed storage, no allocation.
 *
 * Compile condition: CINUX_GUI.
 */

#include "region.hpp"

#include <stdint.h>

namespace cinux::gui {

uint32_t rect_subtract(const Rect& a, const Rect& b, Rect out[4]) {
    if (a.empty()) {
        return 0;
    }
    const Rect isect = rect_intersect(a, b);
    /* No overlap -> a is unchanged. */
    if (isect.empty()) {
        out[0] = a;
        return 1;
    }

    uint32_t n = 0;

    /* Top band: [a.x0, a.x1) x [a.y0, isect.y0) -- the part of a above b. */
    if (isect.y0 > a.y0) {
        out[n++] = Rect{a.x0, a.y0, a.x1, isect.y0};
    }
    /* Bottom band: [a.x0, a.x1) x [isect.y1, a.y1) -- the part of a below b. */
    if (isect.y1 < a.y1) {
        out[n++] = Rect{a.x0, isect.y1, a.x1, a.y1};
    }
    /* Left band: [a.x0, isect.x0) x [isect.y0, isect.y1) -- beside b, vertically
     * aligned with the carved-out intersection. */
    if (isect.x0 > a.x0) {
        out[n++] = Rect{a.x0, isect.y0, isect.x0, isect.y1};
    }
    /* Right band: [isect.x1, a.x1) x [isect.y0, isect.y1). */
    if (isect.x1 < a.x1) {
        out[n++] = Rect{isect.x1, isect.y0, a.x1, isect.y1};
    }
    return n;
}

void Region::add(const Rect& r) {
    if (r.empty()) {
        return;
    }
    if (count_ < kMaxRects) {
        rects_[count_++] = r;
        return;
    }
    /* Overflow: collapse the whole region to its bounding box (all current
     * members plus the new rect). Over-approximation, never under-cover. */
    Rect bbox        = bounds();
    bbox             = rect_union(bbox, r);
    count_           = 0;
    rects_[count_++] = bbox;
}

void Region::intersect(const Rect& clip) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < count_; i++) {
        const Rect c = rect_intersect(rects_[i], clip);
        if (!c.empty()) {
            rects_[w++] = c;
        }
    }
    count_ = w;
}

void Region::translate(int32_t dx, int32_t dy) {
    for (uint32_t i = 0; i < count_; i++) {
        rects_[i] = rect_translate(rects_[i], dx, dy);
    }
}

void Region::subtract(const Rect& r) {
    if (r.empty() || count_ == 0) {
        return;
    }
    /* Fragment every member against r. The fragment count can grow up to 4x,
     * so stage into a buffer then collapse to bounds on overflow.  The buffer
     * is HEAP-allocated: Rect[kMaxRects*4] (128 rects ~= 2 KB) on the stack
     * busts a tight kernel stack (the GUI composite path is already deep). */
    Rect*    staged = new Rect[kMaxRects * 4];
    uint32_t n      = 0;
    for (uint32_t i = 0; i < count_; i++) {
        n += rect_subtract(rects_[i], r, staged + n);
    }
    if (n <= kMaxRects) {
        for (uint32_t i = 0; i < n; i++) {
            rects_[i] = staged[i];
        }
        count_ = n;
    } else {
        /* Overflow: collapse to bounding box of all fragments. */
        Rect bbox = staged[0];
        for (uint32_t i = 1; i < n; i++) {
            bbox = rect_union(bbox, staged[i]);
        }
        count_           = 0;
        rects_[count_++] = bbox;
    }
    delete[] staged;
}

Rect Region::bounds() const {
    if (count_ == 0) {
        return Rect{0, 0, 0, 0};
    }
    Rect b = rects_[0];
    for (uint32_t i = 1; i < count_; i++) {
        b = rect_union(b, rects_[i]);
    }
    return b;
}

}  // namespace cinux::gui
