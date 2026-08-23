/**
 * @file core/gui_core.cpp
 * @brief cinux::gui core session -- staging ownership + one pump iteration
 *
 * Host-neutral: ZERO host includes. Owns the staging buffer; render_frame paints
 * into it; region algebra collects the dirty rects; flush pushes each to the
 * host. See gui_core.hpp for the flush-display-model contract.
 *
 * Compile condition: CINUX_GUI.
 */

#include "gui_core.hpp"

#include <stdint.h>

#include "event.hpp"

namespace cinux::gui {
namespace {

/* Largest event payload we ever buffer behind a header (pointer = 18 B). */
constexpr uint16_t kMaxPayload = 24;

}  // namespace

GuiCore::GuiCore(Host* host, uint32_t width, uint32_t height, PixelFormat fmt) : host_(host) {
    const uint32_t bpp = bytes_per_pixel(fmt);
    if (bpp == 0u) {
        return; /* unsupported format -> session unusable, pump() no-ops */
    }
    const uint32_t stride_bytes = width * bpp;
    staging_backing_            = new uint8_t[static_cast<size_t>(stride_bytes) * height];
    staging_.pixels       = staging_backing_;
    staging_.width        = width;
    staging_.height       = height;
    staging_.stride_bytes = stride_bytes;
    staging_.format       = fmt;
}

GuiCore::~GuiCore() {
    delete[] staging_backing_;
}

void GuiCore::pump() {
    if (host_ == nullptr || staging_.pixels == nullptr) {
        return;
    }
    HostCore& hc = host_->core;

    /* 1. Drain all pending input and let the host dispatch each event. The host
     *    deserialises + applies it to its own GUI state; any change surfaces as
     *    dirty rects from render_frame below. */
    if (hc.poll_event != nullptr && hc.dispatch_event != nullptr) {
        alignas(uint32_t) uint8_t buf[sizeof(EventHeader) + kMaxPayload];
        auto*                     hdr     = reinterpret_cast<EventHeader*>(buf);
        const void*               payload = buf + sizeof(EventHeader);
        while (hc.poll_event(host_->ctx, hdr, sizeof(buf))) {
            hc.dispatch_event(host_->ctx, hdr, payload);
        }
    }

    /* 2. Pre-fill the Frame with the CORE-owned staging buffer, then let the
     *    host render into it + report dirty rects. count==0 = idle -> skip the
     *    flush entirely. The rects array is also core-owned (a GuiCore member),
     *    so no per-frame heap allocation. */
    if (hc.render_frame == nullptr || hc.flush == nullptr) {
        return;
    }
    Frame frame{};
    frame.rects     = dirty_rects_;
    frame.max_rects = kMaxDirtyRects;
    frame.pixels    = staging_.pixels; /* CORE owns staging -> host paints HERE */
    frame.stride    = staging_.stride_bytes;
    frame.width     = staging_.width;
    frame.height    = staging_.height;
    frame.format    = staging_.format;
    hc.render_frame(host_->ctx, &frame);

    if (frame.count == 0u) {
        return;
    }

    /* 3. Collect the reported dirty rects through a Region (dedupe / bounding-
     *    box collapse on overflow -- never under-covers). The region algebra is
     *    now on the real render path, ready for P2's diff-based dirty tracking. */
    Region reg;
    for (uint32_t i = 0u; i < frame.count; i++) {
        reg.add(frame.rects[i]);
    }

    /* 4. Flush each region rect from the staging buffer to the display backend.
     *    The host's flush forwards (framebuffer / SPI / DMA) per its backend. */
    const uint32_t n = reg.count();
    for (uint32_t i = 0u; i < n; i++) {
        const Rect& r = reg.rects()[i];
        hc.flush(host_->ctx, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, staging_.pixels,
                 staging_.stride_bytes, staging_.format);
    }
}

}  // namespace cinux::gui
