/**
 * @file kernel/gui/host_cinux.cpp
 * @brief Cinux host adapter -- fills Host for the in-kernel desktop
 *
 * See host_cinux.hpp. Every callback forwards to an existing in-tree
 * facility; no new behaviour is introduced. The poll_event callback is the
 * only non-trivial one: it dequeues a cinux::gui::Event from the unified mouse
 * queue and serialises it into a EventHeader + typed payload so the
 * (host-agnostic) pump body can consume it.
 *
 * Compile condition: CINUX_GUI.
 */

#include "host_cinux.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/drivers/mouse.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/video/framebuffer.hpp"  // Framebuffer (flush target)
#include "kernel/gui/event.hpp"
#include "kernel/gui/gui_init.hpp"        // create_shell_terminal
#include "kernel/gui/terminal.hpp"        // Terminal (render_frame polls it)
#include "kernel/gui/window_manager.hpp"  // WindowManager (dispatch + render)
#include "kernel/lib/kprintf.hpp"         // kvprintf / kprintf
#include "kernel/lib/string.hpp"          // memcpy
#include "kernel/mm/slab.hpp"             // kmalloc / kfree
#include "third_party/Cinux-GUI/core/event.hpp"
#include "third_party/Cinux-GUI/core/event_payload.hpp"
#include "third_party/Cinux-GUI/core/region.hpp"  // cinux::gui::Region (dirty rects -> Rect)

namespace cinux::gui {
namespace {

HostDesktop           g_cinux_desktop{};
Host                   g_cinux_host{};
cinux::drivers::Framebuffer* g_fb = nullptr; /* flush forwards dirty rects here (§4c) */

/* ============================================================
 * L2 Input: dequeue one cinux::gui::Event and serialise it to a cinux::gui event.
 * ============================================================ */
bool cinux_poll_event(void* ctx, EventHeader* out, uint16_t out_cap) {
    (void)ctx;
    if (out == nullptr || out_cap < sizeof(EventHeader)) {
        return false;
    }

    Event ev;
    if (!cinux::drivers::Mouse::event_queue().dequeue(ev)) {
        return false;
    }

    out->magic   = kEventMagic;
    out->version = kAbiVersion;
    out->flags   = 0;

    uint8_t* tail  = reinterpret_cast<uint8_t*>(out) + sizeof(EventHeader);
    uint16_t avail = static_cast<uint16_t>(out_cap - sizeof(EventHeader));

    switch (ev.type_) {
    case EventType::MouseMove:
    case EventType::MouseDown:
    case EventType::MouseUp: {
        if (avail < sizeof(PointerPayload)) {
            return false;
        }
        out->type = EventCode::kPointer;
        PointerPayload p;
        p.kind    = (ev.type_ == EventType::MouseDown) ? kPointerKindDown
                    : (ev.type_ == EventType::MouseUp) ? kPointerKindUp
                                                       : kPointerKindMove;
        p.x       = ev.mouse.x;
        p.y       = ev.mouse.y;
        p.dx      = ev.mouse.dx;
        p.dy      = ev.mouse.dy;
        p.buttons = ev.mouse.buttons;
        memcpy(tail, &p, sizeof(p));
        out->payload_len = static_cast<uint16_t>(sizeof(p));
        break;
    }
    case EventType::KeyDown:
    case EventType::KeyUp: {
        if (avail < sizeof(KeycodePayload)) {
            return false;
        }
        out->type  = EventCode::kKeycode;
        /* Derive press/release from the dispatch type_ (not ev.key.pressed) so the
         * switch that accepted this event and the PRESSED flag the deserialiser
         * reads are authoritative from one source -- the two can never diverge
         * even if a producer ever lets type_ and key.pressed disagree. */
        out->flags = (ev.type_ == EventType::KeyDown) ? kEventFlagPressed : 0;
        KeycodePayload k;
        k.ascii     = ev.key.ascii;
        k.scancode  = ev.key.scancode;
        k.modifiers = static_cast<uint8_t>((ev.key.shift ? kKeymodShift : 0u) |
                                           (ev.key.ctrl ? kKeymodCtrl : 0u) |
                                           (ev.key.alt ? kKeymodAlt : 0u));
        memcpy(tail, &k, sizeof(k));
        out->payload_len = static_cast<uint16_t>(sizeof(k));
        break;
    }
    default:
        /* An EventType with no serialiser case is dropped here. Adding a new
         * EventType in event.hpp requires updating BOTH this serialiser and
         * cinux_dispatch_event's deserialiser switch. */
        return false;
    }
    return true;
}

/* ============================================================
 * L4 Frame work: the host side of the host-neutral pump.
 *
 * dispatch_event: deserialise a cinux::gui event (the pump drained it via
 * poll_event) back into a cinux::gui::Event and apply it to the window
 * manager. This is the other half of the poll_event serialiser -- together
 * they exercise the event ABI so the same pump body is host-neutral.
 * render_frame: do all per-frame cinux work (deferred icon spawn, terminal
 * poll, cursor footprint, composite) and report the dirty rects + the staging
 * back buffer. count==0 = idle (nothing changed, the pump flushes nothing).
 * ============================================================ */
void cinux_dispatch_event(void* ctx, const EventHeader* ev, const void* payload) {
    (void)ctx;
    if (ev == nullptr || payload == nullptr || ev->magic != kEventMagic ||
        ev->version != kAbiVersion) {
        return;
    }

    auto&             wm   = WindowManager::instance();
    const uint8_t*    tail = static_cast<const uint8_t*>(payload);
    cinux::gui::Event out;

    switch (ev->type) {
    case EventCode::kPointer: {
        if (ev->payload_len < sizeof(PointerPayload)) {
            return;
        }
        PointerPayload p;
        memcpy(&p, tail, sizeof(p));
        switch (p.kind) {
        case kPointerKindMove:
            out.type_ = EventType::MouseMove;
            break;
        case kPointerKindDown:
            out.type_ = EventType::MouseDown;
            break;
        case kPointerKindUp:
            out.type_ = EventType::MouseUp;
            break;
        default:
            return; /* corrupt kind */
        }
        out.mouse.x       = p.x;
        out.mouse.y       = p.y;
        out.mouse.dx      = p.dx;
        out.mouse.dy      = p.dy;
        out.mouse.buttons = p.buttons;
        out.mouse.left    = (p.buttons & 0x1u) != 0;
        out.mouse.right   = (p.buttons & 0x2u) != 0;
        out.mouse.middle  = (p.buttons & 0x4u) != 0;
        wm.handle_mouse(out);
        return;
    }
    case EventCode::kKeycode: {
        if (ev->payload_len < sizeof(KeycodePayload)) {
            return;
        }
        KeycodePayload k;
        memcpy(&k, tail, sizeof(k));
        const bool pressed = (ev->flags & kEventFlagPressed) != 0;
        out.type_          = pressed ? EventType::KeyDown : EventType::KeyUp;
        out.key.ascii      = k.ascii;
        out.key.scancode   = k.scancode;
        out.key.pressed    = pressed;
        out.key.shift      = (k.modifiers & kKeymodShift) != 0;
        out.key.ctrl       = (k.modifiers & kKeymodCtrl) != 0;
        out.key.alt        = (k.modifiers & kKeymodAlt) != 0;
        wm.handle_key(out);
        return;
    }
    default:
        return; /* event type with no deserialiser -- drop */
    }
}

void cinux_render_frame(void* ctx, Frame* frame) {
    (void)ctx;
    if (frame == nullptr) {
        return;
    }
    frame->count = 0;

    auto& wm = WindowManager::instance();

    /* 1. Deferred desktop-icon click -> spawn a shell (structural change). */
    if (wm.consume_pending_icon_action() == IconAction::OpenShell) {
        create_shell_terminal();
        wm.invalidate_all();
    }

    /* 2. Poll terminal output + refresh each terminal's own canvas. */
    for (uint32_t i = 0; i < wm.window_count(); i++) {
        Window* win = wm.window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<Terminal*>(win);
            if (term->poll_output()) {
                wm.invalidate_all();
            }
            if (term->consume_content_dirty()) {
                wm.invalidate_all(); /* pipe-less terminal direct key echo (§4c) */
            }
            term->render_to_canvas();
        }
    }

    /* 3. Cursor footprint (mark old + new rects if the mouse moved). */
    wm.invalidate_cursor_move();

    /* 4. Idle: nothing changed -> skip composite, report no dirty rects. */
    if (wm.dirty().empty()) {
        return;
    }

    /* 5. Render the frame into the staging back buffer. */
    wm.composite();

    cinux::drivers::Canvas* screen = wm.screen();
    if (screen == nullptr || screen->back_buffer() == nullptr) {
        return;
    }

    /* 6. Report dirty rects + staging layout. If the region has more rects than
     *    the core's buffer, collapse to the bounding box (over-cover, never
     *    under-cover). The region already self-collapses at kMaxRects, so this
     *    is just defense. */
    const cinux::gui::Region& dirty = wm.dirty();
    uint32_t             n     = dirty.count();
    if (n > frame->max_rects) {
        const cinux::gui::Rect b = dirty.bounds();
        frame->rects[0]     = Rect{b.x0, b.y0, b.x1, b.y1};
        n                   = 1;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            const cinux::gui::Rect& r = dirty.rects()[i];
            frame->rects[i]      = Rect{r.x0, r.y0, r.x1, r.y1};
        }
    }
    frame->count  = n;
    frame->pixels = screen->back_buffer();
    frame->stride = screen->pitch();
    frame->width  = screen->width();
    frame->height = screen->height();
    frame->format = PixelFormat::kXrgb8888;

    wm.clear_dirty();
}
uint32_t cinux_now_ms(void* ctx) {
    (void)ctx;
    return static_cast<uint32_t>(cinux::drivers::PIT::get_uptime_ms());
}

/* ============================================================
 * Memory / log.
 * ============================================================ */
void* cinux_alloc(void* ctx, size_t n) {
    (void)ctx;
    return cinux::mm::kmalloc(n);
}

void cinux_free(void* ctx, void* p) {
    (void)ctx;
    cinux::mm::kfree(p);
}

__attribute__((format(printf, 2, 3))) void cinux_log(void* ctx, const char* fmt, ...) {
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    cinux::lib::kvprintf(fmt, ap);
    va_end(ap);
}

/* ============================================================
 * L1 Display: flush a dirty rect from the staging buffer to the framebuffer.
 *
 * §4c: the cinux::gui pump renders into the screen canvas's back buffer (the
 * staging Surface) and pushes only the dirty rects through this callback.
 * @p pixels is the staging buffer base; the rect at display coords (x,y,w,h)
 * lives at row offset y*stride + x*4 within it. We copy each row into the VBE
 * framebuffer (volatile MMIO), respecting its own pitch (VBE line alignment
 * may exceed width*4). This replaces the old Canvas::flip() full-frame copy --
 * identical pixels, but only the dirty rects are transferred and the display
 * path now runs through the Host ABI (host-agnostic).
 * ============================================================ */
void cinux_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                 PixelFormat fmt) {
    (void)ctx;
    if (g_fb == nullptr || pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    if (fmt != PixelFormat::kXrgb8888) {
        return; /* Desktop framebuffer is 32bpp XRGB; other formats arrive later */
    }

    const uint32_t fb_pitch = g_fb->pitch();
    const uint32_t fb_w     = g_fb->width();
    const uint32_t fb_h     = g_fb->height();
    /* Clamp the rect to the framebuffer: a rect larger than the screen is
     * ill-formed, and this also bounds w before the *4u below so a pathological
     * caller (the flush is the single hard host seam) cannot wrap bytes_per_row. */
    if (w > static_cast<int>(fb_w)) {
        w = static_cast<int>(fb_w);
    }
    if (h > static_cast<int>(fb_h)) {
        h = static_cast<int>(fb_h);
    }
    const uint8_t*    src           = static_cast<const uint8_t*>(pixels);
    volatile uint8_t* dst           = reinterpret_cast<volatile uint8_t*>(g_fb->data());
    const uint32_t    bytes_per_row = static_cast<uint32_t>(w) * 4u;

    for (int row = 0; row < h; row++) {
        const int py = y + row;
        /* Dirty rects are clipped to screen bounds by WindowManager::invalidate,
         * but defend against a misbehaving caller regardless. */
        if (py < 0 || static_cast<uint32_t>(py) >= fb_h || x < 0 ||
            static_cast<uint32_t>(x) >= fb_w) {
            continue;
        }
        const uint32_t cols = (static_cast<uint32_t>(x) + bytes_per_row / 4u <= fb_w)
                                  ? bytes_per_row
                                  : (fb_w - static_cast<uint32_t>(x)) * 4u;
        const uint8_t* srow = src + static_cast<size_t>(py) * stride + static_cast<size_t>(x) * 4u;
        volatile uint8_t* drow =
            dst + static_cast<size_t>(py) * fb_pitch + static_cast<size_t>(x) * 4u;
        for (uint32_t b = 0; b < cols; b++) {
            drow[b] = srow[b];
        }
    }
}

/* ============================================================
 * Desktop extension: spawn.
 *
 * §3b: forwards to the in-tree shell-terminal helper. path / argv / fd are
 * accepted for ABI completeness but ignored -- a generic spawn(path, argv)
 * returning real stdio handles is §4+ work. Today there is exactly one
 * desktop action (open a shell), and create_shell_terminal() does exactly
 * that, so behaviour matches the non-cinux::gui gui_pump() path.
 * ============================================================ */
int cinux_spawn(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd) {
    (void)ctx;
    (void)path;
    (void)argv;
    if (stdin_fd != nullptr) {
        *stdin_fd = -1;
    }
    if (stdout_fd != nullptr) {
        *stdout_fd = -1;
    }
    create_shell_terminal();
    return 0;
}

}  // namespace

Host& cinux_host() {
    return g_cinux_host;
}

void cinux_host_init(cinux::drivers::Framebuffer* fb) {
    g_fb                               = fb;
    g_cinux_host.core.poll_event       = cinux_poll_event;
    g_cinux_host.core.dispatch_event   = cinux_dispatch_event;
    g_cinux_host.core.render_frame     = cinux_render_frame;
    g_cinux_host.core.flush            = cinux_flush;
    g_cinux_host.core.flush_complete   = nullptr; /* Desktop uses sync flush */
    g_cinux_host.core.now_ms           = cinux_now_ms;
    g_cinux_host.core.alloc            = cinux_alloc;
    g_cinux_host.core.free             = cinux_free;
    g_cinux_host.core.log              = cinux_log;

    g_cinux_desktop.spawn = cinux_spawn;
    g_cinux_host.desktop  = &g_cinux_desktop;
    g_cinux_host.ctx      = nullptr;

    cinux::lib::kprintf("[gui] Cinux host ABI adapter initialised\n");
}

}  // namespace cinux::gui
