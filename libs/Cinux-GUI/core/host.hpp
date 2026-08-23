/**
 * @file core/host.hpp
 * @brief cinux::gui Host ABI -- the ONLY hard seam between the core and any host
 *
 * The core never touches framebuffer / IRQ / syscall / process structures
 * directly -- it only calls through this table. Swapping host (Cinux kernel /
 * future user-space server / SDL / X11 / Wayland) = swapping the table fill.
 *
 * Pure C++17 (stdint/stddef only). PC hosts only (MCU profile dropped, so the
 * C / extern-C global-struct discipline is gone -- everything lives in cinux::gui).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "event.hpp"
#include "region.hpp"  // Rect (dirty-rect wire shape, shared with region algebra)

namespace cinux::gui {

/* Display pixel format. PC hosts use kXrgb8888 / kArgb8888. */
enum class PixelFormat : uint32_t {
    kXrgb8888 = 1, /* 32bpp, no alpha -- Desktop default */
    kArgb8888 = 2, /* 32bpp, premultiplied alpha */
};

/* Bytes per pixel for a CPU-rasterisable format. 0 means the format has no
 * software raster path today (the staging session is unusable; pump() no-ops
 * rather than trap). */
inline constexpr uint32_t bytes_per_pixel(PixelFormat fmt) {
    return (fmt == PixelFormat::kXrgb8888 || fmt == PixelFormat::kArgb8888) ? 4u : 0u;
}

/* One frame's core<->host contract. The CORE (GuiCore) owns BOTH the staging
 * buffer and the rects buffer: pump() pre-fills pixels/stride/width/height/
 * format from the core-owned staging Surface, then calls render_frame(). The
 * HOST paints the scene into that staging buffer and reports the dirty rects
 * (rects/count). count == 0 means nothing changed -- the core flushes nothing
 * (idle skip). This is the DIRECTIVES A "flush display model": the core owns the
 * staging, the host only ever receives a dirty frame. */
struct Frame {
    Rect*       rects;
    uint32_t    max_rects;
    uint32_t    count;
    void*       pixels; /* core-owned staging base; the host paints HERE */
    uint32_t    stride;
    uint32_t    width;
    uint32_t    height;
    PixelFormat format;
};

/* Core host table -- every host fills this. All callbacks receive the host's
 * opaque @p ctx. NULL pointers are allowed: the pump NULL-checks each callback
 * before dereferencing, so a partially-filled table is safe. */
struct HostCore {
    /* L1 Display backend (flush model). The core owns the staging buffer and
     * pushes each dirty rect to the backend. @p pixels is the BASE of the
     * core-owned staging buffer (not the rect's top-left); x/y/w/h locate the
     * dirty rect in display coords; @p stride is the staging row pitch in bytes.
     * A host MUST provide flush -- NULL means rendered frames never reach the
     * display. */
    void (*flush)(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                  PixelFormat fmt);
    void (*flush_complete)(void* ctx); /* host -> core: last async flush done */

    /* L2 Input backend. Drains one raw event into @p out (capacity @p out_cap);
     * returns false when nothing is queued. */
    bool (*poll_event)(void* ctx, EventHeader* out, uint16_t out_cap);

    /* L4 Frame work. dispatch_event applies one drained event to host GUI state;
     * render_frame paints one frame into the CORE-owned staging buffer (already
     * filled in Frame by pump) and reports the dirty rects (rects/count). The
     * core never sees the host's GUI types -- this is what keeps it host-neutral.
     * NULL = host has no input path / renders nothing. */
    void (*dispatch_event)(void* ctx, const EventHeader* ev, const void* payload);
    void (*render_frame)(void* ctx, Frame* frame);

    /* L2 Time backend. */
    uint32_t (*now_ms)(void* ctx);

    /* Memory / Log (all hosts have these). */
    void* (*alloc)(void* ctx, size_t n);
    void (*free)(void* ctx, void* p);
    void (*log)(void* ctx, const char* fmt, ...);
};

/* Desktop extension: process spawn / rpc. NULL on hosts without spawn. */
struct HostDesktop {
    /* spawn a child process, returning its stdio handles. */
    int (*spawn)(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd);
};

/* Aggregate host descriptor: core (always) + optional desktop extension. */
struct Host {
    HostCore     core;
    HostDesktop* desktop; /* NULL on hosts without spawn */
    void*        ctx;     /* opaque host context passed to every callback */
};

}  // namespace cinux::gui
