/**
 * @file user/cinux_gui_host/main.cpp
 * @brief Cinux userspace GUI host (F-GUI-USERSPACE b3a/b3b/b4)
 *
 * b4: full desktop (DesktopIcon Shell/Calculator + cursor) + Shell icon spawn
 * /bin/sh on a Cinux PTY (open /dev/ptmx + fork + execve + TerminalWidget +
 * PTY drain + keyboard→master). User clicks Shell → terminal window → gcc
 * compiles in the GUI.
 *
 * Widget tree mirrors Cinux-GUI host/linux_fbdev_main.cpp pump +
 * host/posix_spawn.cpp PTY (Cinux manual path: no libc forkpty -- open
 * /dev/ptmx + ioctl TIOCGPTN + fork + child opens /dev/pts/N). The kernel
 * host_cinux.cpp adapter was deleted in F-GUI-USERSPACE: this userspace host is
 * the sole GUI host now.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "event.hpp"          // EventHeader, EventCode, kEventMagic, kAbiVersion
#include "event_payload.hpp"  // PointerPayload, KeycodePayload, kPointerKind*, kKeymod*
#include "font.hpp"
#include "gui_core.hpp"
#include "host.hpp"
#include "icon_data.hpp"  // icons::data::k_shell_icon/k_calc_icon + masks
#include "region.hpp"
#include "swraster.hpp"
#include "theme.hpp"
#include "widget.hpp"
#include "widget/desktop_icon.hpp"
#include "widget/label.hpp"
#include "widget/terminal.hpp"
#include "widget/window.hpp"
#include "widget/window_manager.hpp"

// Mirror of kernel/drivers/video/fb_dev.hpp kFbioGetScreenInfo + FbScreenInfo.
#define FBIOGET_SCREENINFO 0x4600
struct fb_screen_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

// Cinux PTY ioctl (Linux TIOCGPTN); musl <sys/ioctl.h> may not expose it.
#ifndef TIOCGPTN
#    define TIOCGPTN 0x80045430
#endif

namespace {

using namespace cinux::gui;

constexpr uint32_t kWinW     = 200;
constexpr uint32_t kWinH     = 160;
constexpr uint32_t kTermCols = 80;
constexpr uint32_t kTermRows = 25;

// Mirror of kernel/gui/event.hpp cinux::gui::Event (/dev/event0 wire format).
enum kernel_event_type : uint8_t {
    kMove      = 0,
    kMouseDown = 1,
    kMouseUp   = 2,
    kKeyDown   = 3,
    kKeyUp     = 4,
};
struct kernel_mouse_event {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    bool    left;
    bool    right;
    bool    middle;
};
struct kernel_key_event {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};
struct kernel_event {
    uint8_t type_;
    union {
        kernel_mouse_event mouse;
        kernel_key_event   key;
    };
};

struct HostState {
    uint8_t*       fb;
    uint32_t       fb_w;
    uint32_t       fb_h;
    uint32_t       fb_pitch;
    int            ev_fd;
    PsfFont        font;
    Theme          theme;
    WindowManager  wm;
    Window         win;
    Label          content;
    DesktopIcon    shell_icon;
    DesktopIcon    calc_icon;
    Desktop        desktop;
    // v1.0.0: multiple shell terminals (one PTY + window per Shell icon click).
    struct ShellSession {
        Window         win;
        TerminalWidget term;
        int            master_fd = -1;  // -1 = free slot
    };
    static constexpr uint32_t kMaxShells = 4;
    ShellSession shells_[kMaxShells];
    int          focus_slot = -1;  // index into shells_[] receiving keyboard
};

// File-scope HostState (BSS) -- NOT a function-local static, to avoid emitting
// __cxa_guard (no libstdc++ in this freestanding link). Widget/PsfFont default
// ctors are side-effect-free (the deleted kernel host adapter relied on the same).
HostState st;

void host_log(void* /*ctx*/, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);  // b4: force flush (stdout may be fully-buffered if not a tty)
}

// L1 Display: flush one dirty rect from staging to the mmap'd framebuffer.
void host_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                PixelFormat fmt) {
    auto* st = static_cast<HostState*>(ctx);
    if (st->fb == nullptr || pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    if (fmt != PixelFormat::kXrgb8888) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0 || x >= static_cast<int>(st->fb_w) || y >= static_cast<int>(st->fb_h)) {
        return;
    }
    if (x + w > static_cast<int>(st->fb_w)) {
        w = static_cast<int>(st->fb_w) - x;
    }
    if (y + h > static_cast<int>(st->fb_h)) {
        h = static_cast<int>(st->fb_h) - y;
    }
    const uint32_t  src_stride_px = stride / 4u;
    const uint32_t  dst_stride_px = st->fb_pitch / 4u;
    const uint32_t* src           = static_cast<const uint32_t*>(pixels);
    uint32_t*       dst           = reinterpret_cast<uint32_t*>(st->fb);
    for (int row = 0; row < h; ++row) {
        const uint32_t  py = static_cast<uint32_t>(y + row);
        const uint32_t* srow =
            src + static_cast<size_t>(py) * src_stride_px + static_cast<uint32_t>(x);
        uint32_t* drow = dst + static_cast<size_t>(py) * dst_stride_px + static_cast<uint32_t>(x);
        for (int p = 0; p < w; ++p) {
            drow[p] = srow[p];
        }
    }
}

// L4 render: drain PTY master → TerminalWidget, then paint the widget tree.
void host_render_frame(void* ctx, Frame* frame) {
    auto* st = static_cast<HostState*>(ctx);
    if (frame == nullptr) {
        return;
    }
    // drain every active shell's PTY → its TerminalWidget (v1.0.0: multi-terminal).
    char buf[512];
    for (uint32_t i = 0; i < HostState::kMaxShells; ++i) {
        if (st->shells_[i].master_fd < 0) {
            continue;
        }
        ssize_t n;
        while ((n = read(st->shells_[i].master_fd, buf, sizeof(buf))) > 0) {
            st->shells_[i].term.write(buf, static_cast<uint32_t>(n));
        }
    }
    // [DIAG] (removed) scroll/drain instrumentation -- kept the host_render_frame
    // drain path clean for the commit.
    Surface s{frame->pixels, frame->width, frame->height, frame->stride, frame->format};
    Region  dirty;
    st->desktop.render(s, st->font, &dirty);
    // b4 fix: force full-screen dirty. Core's WindowManager::remove_window
    // doesn't invalidate the closed window's rect (core bug), so a closed
    // window leaves fb residue until the cursor moves over it. Full flush each
    // frame is cheap on QEMU; the proper fix (core invalidate) is a follow-up.
    if (frame->max_rects >= 1u) {
        frame->rects[0] = Rect{0, 0, static_cast<int32_t>(frame->width),
                               static_cast<int32_t>(frame->height)};
        frame->count    = 1u;
    }
}

// L2 Input: drain one event from /dev/event0 (poll(0) non-blocking).
bool host_poll_event(void* ctx, EventHeader* out, uint16_t cap) {
    auto* st = static_cast<HostState*>(ctx);
    if (out == nullptr || st->ev_fd < 0 || cap < sizeof(EventHeader)) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd      = st->ev_fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    // Nonblocking (timeout=0): pump stays fully responsive -- mouse/keyboard
    // events are read with no latency.  A 10ms blocking wait was tried (to
    // release CPU during gcc compiles) but reverted: it didn't fix the real
    // stall (NVMe sync poll under NvmeBlockDevice::lock_, can't yield) and
    // only added input lag.  The compile-time stall is a known v1.0.0 issue;
    // the cure is async IO (v1.1+).
    if (poll(&pfd, 1, 0) <= 0) {
        return false;
    }
    kernel_event kev;
    ssize_t      n = read(st->ev_fd, &kev, sizeof(kev));
    if (n != static_cast<ssize_t>(sizeof(kev))) {
        return false;
    }
    out->magic   = kEventMagic;
    out->version = kAbiVersion;
    out->flags   = 0;
    auto* tail   = reinterpret_cast<uint8_t*>(out) + sizeof(EventHeader);
    switch (kev.type_) {
    case kMove:
    case kMouseDown:
    case kMouseUp: {
        if (cap < sizeof(EventHeader) + sizeof(PointerPayload)) {
            return false;
        }
        out->type = EventCode::kPointer;
        PointerPayload p;
        p.kind    = (kev.type_ == kMouseDown) ? kPointerKindDown
                    : (kev.type_ == kMouseUp) ? kPointerKindUp
                                              : kPointerKindMove;
        p.x       = kev.mouse.x;
        p.y       = kev.mouse.y;
        p.dx      = kev.mouse.dx;
        p.dy      = kev.mouse.dy;
        p.buttons = kev.mouse.buttons;
        memcpy(tail, &p, sizeof(p));
        out->payload_len = sizeof(p);
        break;
    }
    case kKeyDown:
    case kKeyUp: {
        if (cap < sizeof(EventHeader) + sizeof(KeycodePayload)) {
            return false;
        }
        out->type  = EventCode::kKeycode;
        out->flags = (kev.type_ == kKeyDown) ? kEventFlagPressed : 0;
        KeycodePayload k;
        k.ascii     = kev.key.ascii;
        k.scancode  = kev.key.scancode;
        k.modifiers = static_cast<uint8_t>((kev.key.shift ? kKeymodShift : 0u) |
                                           (kev.key.ctrl ? kKeymodCtrl : 0u) |
                                           (kev.key.alt ? kKeymodAlt : 0u));
        memcpy(tail, &k, sizeof(k));
        out->payload_len = sizeof(k);
        break;
    }
    default:
        return false;
    }
    return true;
}

// L4 dispatch: pointer → WM (cursor/drag/icon-click); keycode → shell PTY.
void host_dispatch_event(void* ctx, const EventHeader* ev, const void* payload) {
    auto* st = static_cast<HostState*>(ctx);
    if (ev == nullptr || payload == nullptr || ev->magic != kEventMagic) {
        return;
    }
    if (ev->type == EventCode::kPointer) {
        if (ev->payload_len < sizeof(PointerPayload)) {
            return;
        }
        PointerPayload p;
        memcpy(&p, payload, sizeof(p));
        st->wm.process_pointer(p);  // cursor + drag + icon click (on_activate)
        // v1.0.0 click-to-focus: a press inside a terminal window's content
        // makes that shell receive keyboard.  Walk newest-first so the
        // top-most overlapping terminal wins (spawn order == z-order here).
        if (p.kind == kPointerKindDown) {
            for (int32_t si = static_cast<int32_t>(HostState::kMaxShells) - 1; si >= 0; --si) {
                if (st->shells_[si].master_fd >= 0 &&
                    st->shells_[si].win.content_rect().contains(p.x, p.y)) {
                    st->focus_slot = si;
                    break;
                }
            }
        }
    } else if (ev->type == EventCode::kKeycode) {
        if (ev->payload_len < sizeof(KeycodePayload)) {
            return;
        }
        KeycodePayload k;
        memcpy(&k, payload, sizeof(k));
        // route key to the focused shell PTY (v1.0.0: multi-terminal; focus =
        // most-recently-spawned shell until click-to-focus is wired).
        const int focus = st->focus_slot;
        if (focus >= 0 && focus < static_cast<int>(HostState::kMaxShells) &&
            st->shells_[focus].master_fd >= 0 && (ev->flags & kEventFlagPressed)) {
            const int fd = st->shells_[focus].master_fd;
            if (k.ascii == '\r' || k.ascii == '\n') {
                write(fd, "\r", 1);
            } else if (k.scancode == 0x0e) {         // set-1 Backspace
                write(fd, "\x7f", 1);  // DEL (line discipline)
            } else if (k.scancode == 0x52) {         // HID Up arrow    -> ESC [ A
                write(fd, "\x1b[A", 3);
            } else if (k.scancode == 0x51) {         // HID Down arrow  -> ESC [ B
                write(fd, "\x1b[B", 3);
            } else if (k.scancode == 0x4f) {         // HID Right arrow -> ESC [ C
                write(fd, "\x1b[C", 3);
            } else if (k.scancode == 0x50) {         // HID Left arrow  -> ESC [ D
                write(fd, "\x1b[D", 3);
            } else if (k.ascii != 0) {
                char c = k.ascii;
                write(fd, &c, 1);
            }
        }
    }
}

uint32_t host_now_ms(void* /*ctx*/) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
void* host_alloc(void* /*ctx*/, size_t n) {
    return malloc(n);
}
void host_free(void* /*ctx*/, void* p) {
    free(p);
}

// WM fires this after remove_window(): close the PTY master of the matching
// shell slot and free it.  If the closed shell held keyboard focus, fall back
// to another active shell so input isn't lost.
void on_win_removed(void* ctx, Window* w) {
    auto* st = static_cast<HostState*>(ctx);
    for (uint32_t i = 0; i < HostState::kMaxShells; ++i) {
        if (w == &st->shells_[i].win && st->shells_[i].master_fd >= 0) {
            close(st->shells_[i].master_fd);
            st->shells_[i].master_fd = -1;  // free the slot for reuse
            if (st->focus_slot == static_cast<int>(i)) {
                st->focus_slot = -1;
                for (uint32_t j = 0; j < HostState::kMaxShells; ++j) {
                    if (st->shells_[j].master_fd >= 0) {
                        st->focus_slot = static_cast<int>(j);
                        break;
                    }
                }
            }
            break;
        }
    }
}

// Shell icon click → spawn /bin/sh on a fresh PTY + open a terminal window.
// v1.0.0: up to kMaxShells concurrent terminals (was single-slot, which is why
// the 2nd click did nothing).  Each click grabs a free slot, spawns, and
// staggers the window so concurrent terminals don't fully overlap.
void shell_activate(void* ctx, DesktopIcon* /*self*/) {
    auto* st = static_cast<HostState*>(ctx);
    int slot = -1;
    for (uint32_t i = 0; i < HostState::kMaxShells; ++i) {
        if (st->shells_[i].master_fd < 0) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        host_log(st, "[gui] shell_activate: no free slot (max %u shells)",
                 HostState::kMaxShells);
        return;
    }
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) {
        host_log(st, "[gui] shell: open /dev/ptmx failed");
        return;
    }
    unsigned int pty_num = 0;
    if (ioctl(master, TIOCGPTN, &pty_num) < 0) {
        int saved_errno = errno;
        host_log(st, "[gui] shell: TIOCGPTN failed errno=%d master=%d", saved_errno, master);
        close(master);
        return;
    }
    char slave[32];
    snprintf(slave, sizeof(slave), "/dev/pts/%u", pty_num);
    pid_t pid = fork();
    if (pid == 0) {
        // child: slave becomes stdio, then exec /bin/sh.
        close(master);
        // Start a fresh session so the shell leads its own process group with no
        // controlling tty.  Without this the shell would inherit the GUI host's
        // pgrp, and a ^C in the terminal would signal the whole desktop.  The
        // slave is then acquired as the controlling terminal so busybox sh can
        // drive job control (TIOCSPGRP) and ^C reaches the foreground job.
        setsid();
        int sfd = open(slave, O_RDWR);
        if (sfd >= 0) {
            dup2(sfd, 0);
            dup2(sfd, 1);
            dup2(sfd, 2);
            if (sfd > 2) {
                close(sfd);
            }
            ioctl(0, TIOCSCTTY, 0);  // 0 is now the slave; acquire it as ctty
        }
        char* argv[] = {const_cast<char*>("sh"), nullptr};
        char* envp[] = {const_cast<char*>("TERM=xterm-256color"),
                        const_cast<char*>("PATH=/bin:/sbin:/usr/bin:/usr/sbin"),
                        nullptr};
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    if (pid < 0) {
        close(master);
        host_log(st, "[gui] shell: fork failed");
        return;
    }
    fcntl(master, F_SETFL, O_NONBLOCK);
    auto& s = st->shells_[slot];
    s.master_fd = master;
    s.term.set_cols_rows(kTermCols, kTermRows);
    s.term.set_theme(&st->theme);
    // stagger each window so concurrent terminals don't fully overlap.
    constexpr int32_t kStagger = 24;
    s.win.set_title("Shell");
    s.win.set_theme(&st->theme);
    s.win.set_rect(80 + slot * kStagger, 60 + slot * kStagger,
                   kTermCols * TerminalWidget::kGlyphW,
                   kTermRows * TerminalWidget::kGlyphH + Window::kTitleBarHeight);
    s.win.set_content(&s.term);
    s.win.layout();
    st->wm.add_window(&s.win);
    st->focus_slot = slot;  // newest terminal takes the keyboard
    host_log(st, "[gui] shell spawned slot=%d/%u pid=%d master=%d", slot,
             HostState::kMaxShells, static_cast<int>(pid), master);
}

void calc_activate(void* /*ctx*/, DesktopIcon* /*self*/) {
    // stub (calculator not wired in b4)
}

}  // namespace

int main(int argc, char** argv) {
    // b4: redirect stdout/stderr to /dev/console so host_log (printf) reaches
    // the serial log. The fork+execve'd host inherits kernel_init's fd table,
    // which may not wire stdout to the console; open it explicitly + fflush.
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) {
        dup2(cfd, 1);
        dup2(cfd, 2);
        if (cfd > 2) {
            close(cfd);
        }
    }

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        return 1;
    }
    struct fb_screen_info info;
    if (ioctl(fb_fd, FBIOGET_SCREENINFO, &info) != 0) {
        return 2;
    }
    size_t   sz = static_cast<size_t>(info.pitch) * static_cast<size_t>(info.height);
    uint8_t* fb =
        static_cast<uint8_t*>(mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0));
    if (fb == reinterpret_cast<uint8_t*>(-1)) {
        return 3;
    }
    int ev_fd = open("/dev/event0", O_RDONLY);

    unsigned long n_pump = 1;
    if (argc > 1 && argv[1] != nullptr) {
        const char*   s = argv[1];
        unsigned long v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + static_cast<unsigned long>(*s - '0');
            ++s;
        }
        n_pump = v;
    }

    // HostState st is the file-scope global above (BSS).
    st.fb       = fb;
    st.fb_w     = info.width;
    st.fb_h     = info.height;
    st.fb_pitch = info.pitch;
    st.ev_fd    = ev_fd;
    st.font.init();
    st.theme = material_dark();
    st.wm.set_rect(0, 0, info.width, info.height);
    st.wm.set_theme(&st.theme);
    st.wm.set_on_remove(on_win_removed, &st);

    // "Hello" window (centered).
    const int32_t wx0 = static_cast<int32_t>(info.width) / 2 - static_cast<int32_t>(kWinW) / 2;
    const int32_t wy0 = static_cast<int32_t>(info.height) / 2 - static_cast<int32_t>(kWinH) / 2;
    st.win.set_title("Cinux");
    st.win.set_theme(&st.theme);
    st.win.set_rect(wx0, wy0, kWinW, kWinH);
    st.content.set_text("Hello\nCinux-GUI");
    st.content.set_color(st.theme.on_surface);
    st.win.set_content(&st.content);
    st.win.layout();
    st.wm.add_window(&st.win);

    // b4: DesktopIcons Shell/Calculator.
    constexpr int32_t kIconW = 32;
    constexpr int32_t kIconH = 32 + 18;
    st.shell_icon.set_bitmap(icons::data::k_shell_icon.data(), icons::data::k_shell_mask.data(),
                             32u, 32u);
    st.shell_icon.set_label("Shell");
    st.shell_icon.set_label_color(st.theme.on_surface);
    st.shell_icon.set_rect(40, 40, static_cast<uint32_t>(kIconW), static_cast<uint32_t>(kIconH));
    st.shell_icon.set_on_activate(shell_activate, &st);

    st.calc_icon.set_bitmap(icons::data::k_calc_icon.data(), icons::data::k_calc_mask.data(), 32u,
                            32u);
    st.calc_icon.set_label("Calculator");
    st.calc_icon.set_label_color(st.theme.on_surface);
    st.calc_icon.set_rect(40, 120, static_cast<uint32_t>(kIconW), static_cast<uint32_t>(kIconH));
    st.calc_icon.set_on_activate(calc_activate, &st);

    st.wm.add_icon(&st.shell_icon);
    st.wm.add_icon(&st.calc_icon);
    st.desktop.set_root(&st.wm);

    Host host{};
    host.core.flush          = host_flush;
    host.core.poll_event     = (ev_fd >= 0) ? host_poll_event : nullptr;
    host.core.dispatch_event = (ev_fd >= 0) ? host_dispatch_event : nullptr;
    host.core.render_frame   = host_render_frame;
    host.core.now_ms         = host_now_ms;
    host.core.alloc          = host_alloc;
    host.core.free           = host_free;
    host.core.log            = host_log;
    host.desktop             = nullptr;
    host.ctx                 = &st;

    auto* core = new GuiCore(&host, info.width, info.height, PixelFormat::kXrgb8888);

    unsigned long iter = (n_pump == 0) ? ~0UL : n_pump;
    for (unsigned long i = 0; i < iter; ++i) {
        core->pump();
    }

    int rc = 0;
    if (n_pump > 0) {
        uint32_t        cx = info.width / 2;
        uint32_t        cy = info.height / 2;
        const uint32_t* px = reinterpret_cast<const uint32_t*>(fb) +
                             static_cast<size_t>(cy) * (info.pitch / 4u) + cx;
        if (*px == 0u) {
            rc = 5;
        }
    }

    delete core;
    return rc;
}
