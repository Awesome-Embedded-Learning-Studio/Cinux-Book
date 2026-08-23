/**
 * @file host/linux_fbdev_main.cpp
 * @brief Linux fbdev+evdev host -- the second real host, widget-tree edition (P4-e)
 *
 * Opens /dev/fb0 (mmap display) + /dev/input/event* (evdev pointer) and drives a
 * widget-tree desktop (WindowManager + Window + Label) through the Host ABI
 * table + GuiCore::pump -- the SAME pump path as the P2 Scene edition, but the
 * scene source is now the P4 widget tree. dispatch_event feeds evdev pointer
 * into WM (drag/raise/close + cursor); render_frame paints the WM via
 * Desktop::render into the core-owned staging.
 *
 * P4-e replaced the P2 Scene + Compositor path with the widget tree, so the
 * real host now exercises the same control layer (WindowManager/Window) as the
 * SDL and terminal hosts. dirty is full-screen each frame (P3-a Desktop::render);
 * per-widget dirty + frame diff is a later optimisation (PLAN P4 decision D).
 *
 * Manual smoke ONLY (needs real /dev/fb0 + /dev/input/event*): NOT a ctest. Run
 * under QEMU with a framebuffer + pointing device, e.g.:
 *     timeout 40 ./fbdev-host
 * watch the VNC display, move/drag the mouse, record the result in notes.
 */

#include <unistd.h>  // usleep

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "evdev_device.hpp"
#include "fbdev_device.hpp"
#include "font.hpp"
#include "gui_core.hpp"
#include "host.hpp"
#include "region.hpp"
#include "swraster.hpp"  // Surface
#include "theme.hpp"
#include "widget.hpp"
#include "widget/label.hpp"
#include "widget/window.hpp"
#include "widget/window_manager.hpp"

namespace {

using namespace cinux::gui;

constexpr uint32_t kWinW = 200;
constexpr uint32_t kWinH = 160;

struct HostState {
    FbdevDevice&   fb;
    EvdevDevice&   ev;
    const PsfFont& font;
    Theme          theme;
    WindowManager  wm;
    Window         win;
    Label          content;
    Desktop        desktop;
};

// ---- host callbacks (Host ABI table) ----
bool fbdev_poll_event(void* ctx, EventHeader* out, uint16_t cap) {
    auto* st = static_cast<HostState*>(ctx);
    if (cap < sizeof(EventHeader) + sizeof(PointerPayload)) {
        return false;
    }
    EvdevFrame f = st->ev.read_frame();
    if (!f.valid) {
        return false;
    }
    out->magic   = kEventMagic;
    out->version = kAbiVersion;
    out->type    = EventCode::kPointer;
    out->flags   = 0;
    PointerPayload p;
    p.kind    = f.kind;
    p.x       = f.x;
    p.y       = f.y;
    p.dx      = 0;
    p.dy      = 0;
    p.buttons = f.buttons;
    std::memcpy(reinterpret_cast<uint8_t*>(out) + sizeof(EventHeader), &p, sizeof(p));
    out->payload_len = sizeof(p);
    return true;
}

void fbdev_dispatch_event(void* ctx, const EventHeader* ev, const void* payload) {
    auto* st = static_cast<HostState*>(ctx);
    if (ev->magic != kEventMagic || ev->type != EventCode::kPointer) {
        return;
    }
    PointerPayload p;
    std::memcpy(&p, payload, sizeof(p));
    st->wm.process_pointer(p);  // WM handles drag / raise / close + cursor
}

void fbdev_render_frame(void* ctx, Frame* frame) {
    auto*   st = static_cast<HostState*>(ctx);
    Surface s{frame->pixels, frame->width, frame->height, frame->stride, frame->format};
    Region  dirty;
    st->desktop.render(s, st->font, &dirty);  // widget tree -> staging (full-screen dirty)
    const uint32_t n = dirty.count();
    for (uint32_t i = 0u; i < n && i < frame->max_rects; ++i) {
        frame->rects[i] = dirty.rects()[i];
    }
    frame->count = n;  // 0 = idle -> pump flushes nothing
}

void fbdev_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                 PixelFormat /*fmt*/) {
    auto* st = static_cast<HostState*>(ctx);
    st->fb.blit_rect(x, y, w, h, pixels, stride);
}

Host make_host(HostState& st) {
    Host h{};
    h.core.poll_event     = fbdev_poll_event;
    h.core.dispatch_event = fbdev_dispatch_event;
    h.core.render_frame   = fbdev_render_frame;
    h.core.flush          = fbdev_flush;
    h.desktop             = nullptr;  // no spawn on fbdev host
    h.ctx                 = &st;
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    PsfFont font;
    font.init();
    if (!font.ready()) {
        std::printf("fbdev-host: font init failed\n");
        return 1;
    }

    FbdevDevice fb;
    const char* fb_path = (argc > 1) ? argv[1] : "/dev/fb0";
    if (!fb.open(fb_path)) {
        std::printf("fbdev-host: cannot open %s\n", fb_path);
        return 1;
    }
    std::printf("fbdev-host: fb %ux%u stride %u\n", fb.width(), fb.height(), fb.stride_bytes());

    const char*   evdev_path = (argc > 2) ? argv[2] : "/dev/input/event0";
    const int32_t cx0        = static_cast<int32_t>(fb.width()) / 2;
    const int32_t cy0        = static_cast<int32_t>(fb.height()) / 2;
    EvdevDevice   ev;
    if (!ev.open(evdev_path, cx0, cy0)) {
        std::printf("fbdev-host: cannot open %s\n", evdev_path);
        return 1;
    }

    Theme     theme = material_dark();
    HostState st{fb, ev, font, theme, {}, {}, {}, {}};

    const int32_t wx0 = cx0 - static_cast<int32_t>(kWinW) / 2;
    const int32_t wy0 = cy0 - static_cast<int32_t>(kWinH) / 2;

    st.wm.set_rect(0, 0, fb.width(), fb.height());
    st.wm.set_theme(&st.theme);
    st.win.set_title("Cinux");
    st.win.set_theme(&st.theme);
    st.win.set_rect(wx0, wy0, kWinW, kWinH);
    st.content.set_text("Hello\nCinux-GUI");
    st.content.set_color(st.theme.on_surface);
    st.win.set_content(&st.content);
    st.win.layout();
    st.wm.add_window(&st.win);
    st.desktop.set_root(&st.wm);

    Host    host = make_host(st);
    GuiCore core(&host, fb.width(), fb.height(), fb.format());

    std::printf(
        "fbdev-host: running (widget tree) -- move/drag the mouse (VNC). "
        "External `timeout` or Ctrl-C exits.\n");
    std::fflush(stdout);
    for (;;) {
        core.pump();
        ::usleep(16000);  // ~60 fps cap
    }
    return 0;
}
