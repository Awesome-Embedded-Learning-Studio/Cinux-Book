/**
 * @file host/fake_host_main.cpp
 * @brief Standalone host-neutrality smoke harness for the cinux::gui core
 *
 * Drives GuiCore::pump() with a hand-filled Host table -- NO host, NO
 * framebuffer, NO real input. Built only when this dir is the build root
 * (`cmake -S . -B build && ctest --test-dir build`). If it links and runs green
 * under a standard hosted compiler, the core is provably host-neutral: the same
 * core TUs that drive the Cinux kernel today drive this fake host with only the
 * table fill swapped.
 *
 * This file is the seed of the SDL / X11 / Wayland host adapters: replace the
 * fake_* callbacks with real host input/render/flush and the core runs unchanged.
 *
 * NOT built inside a host tree -- the Cinux kernel has its own in-QEMU tests
 * under kernel/test/.
 */

#include <cstdint>
#include <cstdio>

#include "font.hpp"      // PsfFont
#include "gui_core.hpp"  // GuiCore
#include "host.hpp"      // Host, HostCore, Frame, PixelFormat
#include "region.hpp"    // Rect, Region, rect_intersect
#include "swraster.hpp"  // Surface, fill_rect, glyph_blit

namespace {

using namespace cinux::gui;  // the core types live here

// ---- recorded flush calls (the harness's pretend "display") ----
struct FlushRecord {
    int      x, y, w, h;
    uint32_t stride;
    int      format;
};
constexpr uint32_t kMaxRecorded = 8;
FlushRecord g_flushed[kMaxRecorded];
uint32_t    g_flush_count = 0;

// ---- per-test render behaviour (set before each pump call) ----
enum class RenderMode { Idle, OneRect, TwoRect };
RenderMode g_render_mode  = RenderMode::Idle;
Rect       g_test_rect{};
Rect       g_test_rect2{};
uint32_t   g_paint_marker = 0; /* if non-zero, render_frame writes it to staging[0] */

// ---- fake host callbacks (the table fill a real host swaps) ----
bool fake_poll_event(void* /*ctx*/, EventHeader* /*out*/, uint16_t /*out_cap*/) {
    return false;  // no input queued -- the drain loop does nothing
}

void fake_dispatch_event(void* /*ctx*/, const EventHeader* /*ev*/, const void* /*payload*/) {
    // no-op: nothing to apply in a fake host
}

void fake_render_frame(void* /*ctx*/, Frame* frame) {
    if (g_render_mode == RenderMode::Idle) {
        frame->count = 0;  // idle: nothing changed this iteration
        return;
    }
    /* The core pre-filled frame->pixels with the staging base (core owns it).
     * A real host would paint the scene here with swraster; the harness just
     * optionally drops a marker pixel to prove the staging path is writable AND
     * that pump flushes the core-owned buffer the host painted into. */
    if (g_paint_marker != 0u && frame->pixels != nullptr) {
        static_cast<uint32_t*>(frame->pixels)[0] = g_paint_marker;
    }
    frame->rects[0] = g_test_rect;
    frame->count    = (g_render_mode == RenderMode::TwoRect) ? 2u : 1u;
    if (g_render_mode == RenderMode::TwoRect) {
        frame->rects[1] = g_test_rect2;
    }
}

void fake_flush(void* /*ctx*/, int x, int y, int w, int h, const void* /*pixels*/,
                uint32_t stride, PixelFormat fmt) {
    if (g_flush_count < kMaxRecorded) {
        g_flushed[g_flush_count] = {x, y, w, h, stride, static_cast<int>(fmt)};
    }
    g_flush_count++;
}

Host make_host() {
    Host h{};
    h.core.poll_event     = fake_poll_event;
    h.core.dispatch_event = fake_dispatch_event;
    h.core.render_frame   = fake_render_frame;
    h.core.flush          = fake_flush;
    h.desktop             = nullptr;
    h.ctx                 = nullptr;
    return h;
}

void reset(RenderMode mode, Rect r, Rect r2 = {}, uint32_t marker = 0u) {
    g_render_mode  = mode;
    g_test_rect    = r;
    g_test_rect2   = r2;
    g_paint_marker = marker;
    g_flush_count  = 0;
}

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL: " __VA_ARGS__);                                 \
            std::printf("\n");                                                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int run() {
    // Display geometry for the fake session (small -- flush geometry + the
    // staging-ownership link are under test, not actual pixels).
    constexpr uint32_t kW = 16, kH = 16;

    // 1. NULL host -> GuiCore::pump() is a safe no-op (defensive discipline).
    {
        GuiCore core(nullptr, kW, kH, PixelFormat::kXrgb8888);
        core.pump();
    }

    // 2. Idle frame: render_frame reports count == 0 -> pump flushes nothing.
    {
        Host    h = make_host();
        GuiCore core(&h, kW, kH, PixelFormat::kXrgb8888);
        reset(RenderMode::Idle, {});
        core.pump();
        CHECK(g_flush_count == 0, "idle frame flushed %u times (expected 0)", g_flush_count);
    }

    // 3. Dirty frame: one rect {x0=1,y0=2,x1=3,y1=4} -> flush(1,2,2,2) once.
    {
        Host    h = make_host();
        GuiCore core(&h, kW, kH, PixelFormat::kXrgb8888);
        reset(RenderMode::OneRect, Rect{1, 2, 3, 4});
        core.pump();
        CHECK(g_flush_count == 1, "dirty frame flushed %u times (expected 1)", g_flush_count);
        CHECK(g_flushed[0].x == 1 && g_flushed[0].y == 2 && g_flushed[0].w == 2 &&
                  g_flushed[0].h == 2,
              "flush geometry {%d,%d,%d,%d} (expected 1,2,2,2)", g_flushed[0].x, g_flushed[0].y,
              g_flushed[0].w, g_flushed[0].h);
        CHECK(g_flushed[0].format == static_cast<int>(PixelFormat::kXrgb8888),
              "flush format %d (expected kXrgb8888)", g_flushed[0].format);
    }

    // 4. Staging ownership: render_frame writes a marker into frame->pixels;
    //    after pump the SAME marker is readable via core.staging() -- i.e. the
    //    core owns the buffer the host painted into (not a host-owned one).
    {
        Host    h = make_host();
        GuiCore core(&h, kW, kH, PixelFormat::kXrgb8888);
        constexpr uint32_t kMarker = 0x00ABCDEFu;
        reset(RenderMode::OneRect, Rect{0, 0, 2, 2}, {}, kMarker);
        core.pump();
        const uint32_t* px = static_cast<const uint32_t*>(core.staging().pixels);
        CHECK(px[0] == kMarker, "staging[0] = 0x%08X (expected marker 0x%08X)", px[0], kMarker);
    }

    // 5. Region on the render path: two disjoint reported rects -> two flushes.
    //    Proves the region algebra sits between render_frame and flush (two
    //    disjoint rects are not collapsed, so both reach the display).
    {
        Host    h = make_host();
        GuiCore core(&h, kW, kH, PixelFormat::kXrgb8888);
        reset(RenderMode::TwoRect, Rect{0, 0, 2, 2}, Rect{4, 4, 6, 6});
        core.pump();
        CHECK(g_flush_count == 2, "two-rect frame flushed %u times (expected 2)", g_flush_count);
    }

    // 6. Region algebra (pure logic, no host): intersect + bounded Region add.
    {
        Rect a{0, 0, 10, 10}, b{5, 5, 15, 15};
        Rect isect = rect_intersect(a, b);
        CHECK(isect.x0 == 5 && isect.y0 == 5 && isect.x1 == 10 && isect.y1 == 10,
              "rect_intersect {%d,%d,%d,%d} (expected 5,5,10,10)", isect.x0, isect.y0, isect.x1,
              isect.y1);

        Region reg;
        reg.add(Rect{0, 0, 4, 4});
        reg.add(Rect{10, 10, 14, 14});
        CHECK(reg.count() == 2, "region count %u (expected 2)", reg.count());
    }

    // 7. PSF2 font: parse -> 8x16/256 glyphs; 'A' renders foreground pixels,
    //    ' ' (space) renders none -- proves parse + glyph index + glyph_blit link.
    {
        PsfFont font;
        font.init();
        CHECK(font.ready(), "font not ready (PSF2 parse failed)");
        CHECK(font.width() == 8, "font width %u (expected 8)", font.width());
        CHECK(font.height() == 16, "font height %u (expected 16)", font.height());
        CHECK(font.num_glyphs() == 256, "font num_glyphs %u (expected 256)", font.num_glyphs());

        constexpr uint32_t kFw = 8, kFh = 16;
        uint32_t          fbuf[kFw * kFh];
        Surface           fs{static_cast<void*>(fbuf), kFw, kFh, kFw * 4u, PixelFormat::kXrgb8888};
        constexpr uint32_t kFg = 0x00FFFFFFu;

        fill_rect(fs, 0, 0, kFw, kFh, 0x00000000u, nullptr);
        glyph_blit(fs, 0, 0, font.glyph('A'), font.width(), font.height(), kFg, nullptr);
        uint32_t on_a = 0;
        for (uint32_t i = 0; i < kFw * kFh; i++) {
            if (fbuf[i] == kFg) {
                on_a++;
            }
        }
        CHECK(on_a > 0, "'A' rendered %u fg pixels (expected >0)", on_a);

        fill_rect(fs, 0, 0, kFw, kFh, 0x00000000u, nullptr);
        glyph_blit(fs, 0, 0, font.glyph(' '), font.width(), font.height(), kFg, nullptr);
        uint32_t on_sp = 0;
        for (uint32_t i = 0; i < kFw * kFh; i++) {
            if (fbuf[i] == kFg) {
                on_sp++;
            }
        }
        CHECK(on_sp == 0, "' ' rendered %u fg pixels (expected 0)", on_sp);
    }

    std::printf("cinux-gui-smoke: OK (null-host safe + idle skip + dirty flush + "
                "staging ownership + region-on-path + region algebra + psf2 font)\n");
    return 0;
}

}  // namespace

int main() { return run(); }
