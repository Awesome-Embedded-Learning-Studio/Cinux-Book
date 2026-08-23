/**
 * @file core/gui_core.hpp
 * @brief cinux::gui core session -- owns the staging Surface + drives pump
 *
 * GuiCore is the core-owned state the DIRECTIVES A "flush display model" names:
 * the CORE owns one staging Surface, the host paints the scene into it (via
 * swraster), region algebra collects the dirty rects, and each dirty rect is
 * pushed to the host via flush. The host never owns the staging buffer -- it
 * receives it pre-filled in Frame and paints there. This is the seam that keeps
 * the core host-neutral: GuiCore knows nothing about windows, cursors, or
 * widgets -- only "drain input, hand the host a staging buffer, flush what it
 * dirtied".
 *
 * pump() is the single host-neutral GUI iteration. It has ZERO host includes --
 * everything host-specific (GUI types, renderer, cursor, what counts as
 * "changed") lives in the host adapter that fills the Host table. Swapping host
 * (Cinux kernel / SDL / X11 / Wayland / offscreen test host) = swapping the
 * table fill; the core body never changes.
 *
 * Compile condition: CINUX_GUI.
 *
 * Namespace: cinux::gui
 */
#pragma once

#include <stdint.h>

#include "host.hpp"      // Host, Frame, PixelFormat
#include "region.hpp"    // Rect
#include "swraster.hpp"  // Surface

namespace cinux::gui {

/**
 * @brief Core-owned GUI session: staging Surface + one pump iteration
 *
 * Owns the staging pixel buffer (allocated once at construction, reused across
 * frames -- NOT a per-frame allocation). pump() drains input through the host
 * table, asks the host to render one frame into the staging Surface, collects
 * the reported dirty rects through a Region (dedupe / bounding-box collapse on
 * overflow -- never under-covers), then flushes each region rect to the display.
 *
 * Lifetime: construct once per display, pump() each iteration, destruct on
 * shutdown. Not copyable (owns a heap buffer).
 */
class GuiCore {
public:
    /** Cap on dirty rects the host may report per frame before region collapse. */
    static constexpr uint32_t kMaxDirtyRects = 64;

    /**
     * @brief Construct the session over a filled host table + display geometry
     *
     * Allocates the staging buffer (width*height*bytes_per_pixel(fmt)). A format
     * with no CPU raster path today leaves the session unusable: staging() has a
     * null pixel base and pump() no-ops, rather than trapping.
     *
     * @param host   filled host descriptor (non-owning; NULL -> pump() no-ops)
     * @param width  staging/display width in pixels
     * @param height staging/display height in pixels
     * @param fmt    staging pixel format
     */
    GuiCore(Host* host, uint32_t width, uint32_t height, PixelFormat fmt);
    ~GuiCore();

    GuiCore(const GuiCore&)            = delete;
    GuiCore& operator=(const GuiCore&) = delete;

    /**
     * @brief Run one host-neutral GUI iteration
     *
     * Sequence:
     *   1. Drain all input via host->core.poll_event(); hand each event to
     *      host->core.dispatch_event().
     *   2. Pre-fill Frame with the core-owned staging buffer, then call
     *      host->core.render_frame(): the host paints into staging + reports
     *      dirty rects. count==0 = idle -> skip the flush entirely.
     *   3. Collect the reported rects through a Region (add/dedupe/collapse).
     *   4. Flush each region rect from staging via host->core.flush().
     *
     * Every dereferenced callback is NULL-checked; a partially-filled table is
     * safe. A NULL host (or an unusable staging buffer) makes pump() a no-op.
     */
    void pump();

    /** Read-only view of the core-owned staging Surface (the host paints here). */
    const Surface& staging() const { return staging_; }

    /** The host table this session drives (non-owning; may be NULL). */
    Host* host() const { return host_; }

private:
    Host*    host_;                          // non-owning; NULL -> pump() no-op
    Surface  staging_{};                     // descriptor over staging_backing_
    uint8_t* staging_backing_ = nullptr;     // owned pixel storage (w*h*bpp)
    Rect     dirty_rects_[kMaxDirtyRects];   // host writes via Frame; pump region-collects
};

}  // namespace cinux::gui
