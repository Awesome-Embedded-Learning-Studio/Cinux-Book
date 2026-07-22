/**
 * @file kernel/test/test_gui_dirty.cpp
 * @brief QEMU in-kernel tests for the cinux::gui dirty-region + pump flush path (F13)
 *
 * Two layers (the pump is now a host-neutral shell -- it drains events, calls
 * render_frame, flushes the reported rects; the dirty POLICY lives in the host
 * adapter, exercised by the GUI smoke):
 *   1. WindowManager dirty-mechanism unit tests -- invalidate/invalidate_all/
 *      clipping directly on the Region (no pump).
 *   2. pump flush-loop tests -- a fake host whose render_frame fills rects
 *      + a recording flush; asserts the pump flushes exactly what render_frame
 *      reports and flushes nothing when render_frame reports count==0 (idle).
 *
 * Compile condition: CINUX_GUI.
 */

#include <stdint.h>

#include "big_kernel_test.h"

#ifdef CINUX_GUI
#    include "kernel/drivers/canvas.hpp"
#    include "third_party/Cinux-GUI/core/host.hpp"
#    include "third_party/Cinux-GUI/core/pump.hpp"
#    include "third_party/Cinux-GUI/core/region.hpp"
#    include "kernel/gui/window_manager.hpp"
#endif

#ifdef CINUX_GUI

using cinux::gui::Rect;
using cinux::drivers::Canvas;
using cinux::gui::WindowManager;

namespace {
using namespace cinux::gui;  // bare core types (Host/Frame/Rect/PixelFormat)

/// A recorded flush rect (what the fake host received).
struct FlushRect {
    int x;
    int y;
    int w;
    int h;
};

FlushRect g_flushed[64];
uint32_t  g_flushed_n = 0;

/// Fake host flush: ignore the pixels, just record the rect the pump pushed.
void record_flush(void* /*ctx*/, int x, int y, int w, int h, const void* /*pixels*/,
                  uint32_t /*stride*/, PixelFormat /*fmt*/) {
    if (g_flushed_n < 64) {
        g_flushed[g_flushed_n] = FlushRect{x, y, w, h};
        g_flushed_n++;
    }
}

/// Fake render_frame: report two dirty rects.
void fake_render_two(void* /*ctx*/, Frame* frame) {
    frame->rects[0] = Rect{1, 2, 3, 4};     /* x=1 y=2 w=2 h=2 */
    frame->rects[1] = Rect{10, 20, 30, 40}; /* x=10 y=20 w=20 h=20 */
    frame->count    = 2;
    frame->pixels   = reinterpret_cast<const void*>(0x1); /* non-null so the pump flushes */
    frame->stride   = 4;
    frame->format   = PixelFormat::kXrgb8888;
}

/// Fake render_frame: idle (nothing changed).
void fake_render_idle(void* /*ctx*/, Frame* frame) {
    frame->count = 0;
}

}  // namespace

// ============================================================
// WindowManager dirty-mechanism unit tests (no pump)
// ============================================================

namespace test_gui_dirty_api {

void test_invalidate_adds_clipped_rect() {
    Canvas screen;
    screen.init(100, 100);
    WindowManager wm;
    wm.init(&screen, nullptr);

    wm.invalidate(Rect{10, 10, 20, 20});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    TEST_ASSERT_EQ(wm.dirty().rects()[0].x0, 10);
    TEST_ASSERT_EQ(wm.dirty().rects()[0].y1, 20);

    /* Off-screen rect is clipped to empty and dropped. */
    wm.invalidate(Rect{200, 200, 300, 300});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);

    wm.clear_dirty();
    TEST_ASSERT_TRUE(wm.dirty().empty());
}

void test_invalidate_all_covers_screen() {
    Canvas screen;
    screen.init(80, 60);
    WindowManager wm;
    wm.init(&screen, nullptr);

    wm.invalidate_all();
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    Rect b = wm.dirty().bounds();
    TEST_ASSERT_EQ(b.x0, 0);
    TEST_ASSERT_EQ(b.y0, 0);
    TEST_ASSERT_EQ(b.x1, 80);
    TEST_ASSERT_EQ(b.y1, 60);
}

void test_invalidate_clips_partial_offscreen() {
    Canvas screen;
    screen.init(50, 50);
    WindowManager wm;
    wm.init(&screen, nullptr);

    wm.invalidate(Rect{40, 40, 200, 200});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    Rect r = wm.dirty().rects()[0];
    TEST_ASSERT_EQ(r.x0, 40);
    TEST_ASSERT_EQ(r.y0, 40);
    TEST_ASSERT_EQ(r.x1, 50);
    TEST_ASSERT_EQ(r.y1, 50);
}

}  // namespace test_gui_dirty_api

// ============================================================
// pump flush-loop tests (host-neutral pump + fake host)
// ============================================================

namespace test_pump_flush {

void test_pump_flushes_rendered_rects() {
    g_flushed_n = 0;
    Host h{};
    h.core.render_frame = fake_render_two;
    h.core.flush        = record_flush;
    cinux::gui::pump(&h);

    TEST_ASSERT_EQ(g_flushed_n, 2u);
    TEST_ASSERT_EQ(g_flushed[0].x, 1);
    TEST_ASSERT_EQ(g_flushed[0].y, 2);
    TEST_ASSERT_EQ(g_flushed[0].w, 2);
    TEST_ASSERT_EQ(g_flushed[0].h, 2);
    TEST_ASSERT_EQ(g_flushed[1].x, 10);
    TEST_ASSERT_EQ(g_flushed[1].y, 20);
    TEST_ASSERT_EQ(g_flushed[1].w, 20);
    TEST_ASSERT_EQ(g_flushed[1].h, 20);
}

void test_pump_idle_flushes_nothing() {
    g_flushed_n = 0;
    Host h{};
    h.core.render_frame = fake_render_idle;
    h.core.flush        = record_flush;
    cinux::gui::pump(&h);

    TEST_ASSERT_EQ(g_flushed_n, 0u);
}

}  // namespace test_pump_flush

extern "C" void run_gui_dirty_tests() {
    TEST_SECTION("cinux::gui Dirty/Flush Tests (F13)");
    RUN_TEST(test_gui_dirty_api::test_invalidate_adds_clipped_rect);
    RUN_TEST(test_gui_dirty_api::test_invalidate_all_covers_screen);
    RUN_TEST(test_gui_dirty_api::test_invalidate_clips_partial_offscreen);
    RUN_TEST(test_pump_flush::test_pump_flushes_rendered_rects);
    RUN_TEST(test_pump_flush::test_pump_idle_flushes_nothing);
    TEST_SUMMARY();
}

#else /* !CINUX_GUI */

extern "C" void run_gui_dirty_tests() {}

#endif /* CINUX_GUI */
