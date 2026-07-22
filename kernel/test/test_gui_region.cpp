/**
 * @file kernel/test/test_gui_region.cpp
 * @brief QEMU in-kernel tests for the cinux::gui region algebra (F13 §4b)
 *
 * Pure-logic tests: each builds Rects / a Region, runs an operation, and
 * asserts exact results. The degenerate (empty) and capacity-collapse paths
 * are exercised explicitly -- those are what keep dirty regions from dropping
 * pixels.
 *
 * Compile condition: CINUX_GUI (region lives under kernel/gui).
 */

#include <stdint.h>

#include "big_kernel_test.h"

#ifdef CINUX_GUI
#    include "third_party/Cinux-GUI/core/region.hpp"
#endif

#ifdef CINUX_GUI

using cinux::gui::Rect;
using cinux::gui::Region;

namespace test_Rect {

void test_intersect_overlap() {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 15, 15};
    Rect r = cinux::gui::rect_intersect(a, b);
    TEST_ASSERT_EQ(r.x0, 5);
    TEST_ASSERT_EQ(r.y0, 5);
    TEST_ASSERT_EQ(r.x1, 10);
    TEST_ASSERT_EQ(r.y1, 10);
    TEST_ASSERT_FALSE(r.empty());
}

void test_intersect_disjoint_degenerate() {
    Rect a{0, 0, 4, 4};
    Rect b{5, 5, 9, 9};
    Rect r = cinux::gui::rect_intersect(a, b);
    TEST_ASSERT_TRUE(r.empty());
}

void test_union_envelope() {
    Rect a{0, 0, 4, 4};
    Rect b{6, 6, 10, 10};
    Rect r = cinux::gui::rect_union(a, b);
    TEST_ASSERT_EQ(r.x0, 0);
    TEST_ASSERT_EQ(r.y0, 0);
    TEST_ASSERT_EQ(r.x1, 10);
    TEST_ASSERT_EQ(r.y1, 10);
}

void test_union_one_empty() {
    Rect a{0, 0, 0, 0};  // degenerate
    Rect b{1, 1, 3, 3};
    Rect r = cinux::gui::rect_union(a, b);
    TEST_ASSERT_EQ(r.x0, 1);
    TEST_ASSERT_EQ(r.y1, 3);
}

void test_translate() {
    Rect a{1, 2, 3, 4};
    Rect r = cinux::gui::rect_translate(a, 10, -5);
    TEST_ASSERT_EQ(r.x0, 11);
    TEST_ASSERT_EQ(r.y0, -3);
    TEST_ASSERT_EQ(r.x1, 13);
    TEST_ASSERT_EQ(r.y1, -1);
}

void test_contains_point() {
    Rect a{0, 0, 4, 4};  // half-open
    TEST_ASSERT_TRUE(a.contains(0, 0));
    TEST_ASSERT_TRUE(a.contains(3, 3));
    TEST_ASSERT_FALSE(a.contains(4, 0));  // right edge excluded
    TEST_ASSERT_FALSE(a.contains(0, 4));  // bottom edge excluded
    TEST_ASSERT_FALSE(a.contains(-1, 0));
}

void test_contains_rect() {
    Rect outer{0, 0, 10, 10};
    TEST_ASSERT_TRUE((outer.contains(Rect{1, 1, 9, 9})));
    TEST_ASSERT_TRUE((outer.contains(Rect{0, 0, 10, 10})));   // exact
    TEST_ASSERT_FALSE((outer.contains(Rect{9, 9, 11, 11})));  // spills out
}

void test_empty_degenerate() {
    TEST_ASSERT_TRUE((Rect{5, 5, 5, 10}.empty()));  // zero width
    TEST_ASSERT_TRUE((Rect{5, 5, 10, 5}.empty()));  // zero height
    TEST_ASSERT_TRUE((Rect{5, 5, 4, 10}.empty()));  // inverted x
    TEST_ASSERT_FALSE((Rect{0, 0, 1, 1}.empty()));
}

}  // namespace test_Rect

namespace test_gui_subtract {

void test_subtract_no_overlap() {
    Rect     a{0, 0, 4, 4};
    Rect     b{10, 10, 20, 20};
    Rect     out[4];
    uint32_t n = cinux::gui::rect_subtract(a, b, out);
    TEST_ASSERT_EQ(n, 1u);
    TEST_ASSERT_EQ(out[0].x0, 0);
    TEST_ASSERT_EQ(out[0].y1, 4);  // a unchanged
}

void test_subtract_hole_all_four() {
    /* b sits in the middle of a -> top/bottom/left/right strips. */
    Rect     a{0, 0, 10, 10};
    Rect     b{3, 3, 7, 7};
    Rect     out[4];
    uint32_t n = cinux::gui::rect_subtract(a, b, out);
    TEST_ASSERT_EQ(n, 4u);
    /* Top band [0,10)x[0,3) */
    TEST_ASSERT_EQ(out[0].x0, 0);
    TEST_ASSERT_EQ(out[0].y0, 0);
    TEST_ASSERT_EQ(out[0].x1, 10);
    TEST_ASSERT_EQ(out[0].y1, 3);
    /* Bottom band [0,10)x[7,10) */
    TEST_ASSERT_EQ(out[1].y0, 7);
    TEST_ASSERT_EQ(out[1].y1, 10);
    /* Left band [0,3)x[3,7) */
    TEST_ASSERT_EQ(out[2].x0, 0);
    TEST_ASSERT_EQ(out[2].x1, 3);
    TEST_ASSERT_EQ(out[2].y0, 3);
    TEST_ASSERT_EQ(out[2].y1, 7);
    /* Right band [7,10)x[3,7) */
    TEST_ASSERT_EQ(out[3].x0, 7);
    TEST_ASSERT_EQ(out[3].x1, 10);
}

void test_subtract_covers_all() {
    /* b fully covers a -> no fragments remain. */
    Rect     a{2, 2, 5, 5};
    Rect     b{0, 0, 10, 10};
    Rect     out[4];
    uint32_t n = cinux::gui::rect_subtract(a, b, out);
    TEST_ASSERT_EQ(n, 0u);
}

void test_subtract_edge_touch() {
    /* b shares the left edge of a -> only right + bottom strips. */
    Rect     a{0, 0, 10, 10};
    Rect     b{0, 0, 4, 4};
    Rect     out[4];
    uint32_t n = cinux::gui::rect_subtract(a, b, out);
    /* No top band (isect.y0 == a.y0); no left band (isect.x0 == a.x0). */
    TEST_ASSERT_EQ(n, 2u);
    /* Bottom band [0,10)x[4,10) */
    TEST_ASSERT_EQ(out[0].y0, 4);
    /* Right band [4,10)x[0,4) */
    TEST_ASSERT_EQ(out[1].x0, 4);
    TEST_ASSERT_EQ(out[1].y0, 0);
    TEST_ASSERT_EQ(out[1].y1, 4);
}

}  // namespace test_gui_subtract

namespace test_gui_region {

void test_region_add_bounds() {
    Region reg;
    TEST_ASSERT_TRUE(reg.empty());
    reg.add(Rect{0, 0, 2, 2});
    reg.add(Rect{10, 10, 12, 12});
    TEST_ASSERT_EQ(reg.count(), 2u);
    Rect b = reg.bounds();
    TEST_ASSERT_EQ(b.x0, 0);
    TEST_ASSERT_EQ(b.y0, 0);
    TEST_ASSERT_EQ(b.x1, 12);
    TEST_ASSERT_EQ(b.y1, 12);
}

void test_region_add_ignores_degenerate() {
    Region reg;
    reg.add(Rect{0, 0, 0, 0});  // ignored
    reg.add(Rect{1, 1, 3, 3});
    TEST_ASSERT_EQ(reg.count(), 1u);
}

void test_region_intersect() {
    Region reg;
    reg.add(Rect{0, 0, 10, 10});
    reg.add(Rect{20, 20, 30, 30});
    reg.intersect(Rect{5, 5, 25, 25});
    TEST_ASSERT_EQ(reg.count(), 2u);
    /* First clipped to [5,10)x[5,10), second to [20,25)x[20,25). */
    TEST_ASSERT_EQ(reg.rects()[0].x0, 5);
    TEST_ASSERT_EQ(reg.rects()[0].x1, 10);
    TEST_ASSERT_EQ(reg.rects()[1].x0, 20);
    TEST_ASSERT_EQ(reg.rects()[1].y1, 25);
}

void test_region_intersect_drops_outside() {
    Region reg;
    reg.add(Rect{0, 0, 4, 4});
    reg.add(Rect{100, 100, 104, 104});
    reg.intersect(Rect{0, 0, 50, 50});  // second rect falls outside
    TEST_ASSERT_EQ(reg.count(), 1u);
    TEST_ASSERT_EQ(reg.rects()[0].x0, 0);
}

void test_region_translate() {
    Region reg;
    reg.add(Rect{0, 0, 2, 2});
    reg.add(Rect{5, 5, 7, 7});
    reg.translate(100, 200);
    TEST_ASSERT_EQ(reg.rects()[0].x0, 100);
    TEST_ASSERT_EQ(reg.rects()[0].y0, 200);
    TEST_ASSERT_EQ(reg.rects()[1].x1, 107);
}

void test_region_subtract() {
    Region reg;
    reg.add(Rect{0, 0, 10, 10});
    reg.subtract(Rect{3, 3, 7, 7});  // carve a hole
    /* One member -> 4 fragments. */
    TEST_ASSERT_EQ(reg.count(), 4u);
    /* Union of fragments must re-cover a minus the hole: bounds still [0,10)^2. */
    Rect b = reg.bounds();
    TEST_ASSERT_EQ(b.x0, 0);
    TEST_ASSERT_EQ(b.y0, 0);
    TEST_ASSERT_EQ(b.x1, 10);
    TEST_ASSERT_EQ(b.y1, 10);
}

void test_region_clear() {
    Region reg;
    reg.add(Rect{0, 0, 2, 2});
    reg.clear();
    TEST_ASSERT_TRUE(reg.empty());
    TEST_ASSERT_EQ(reg.count(), 0u);
}

void test_region_bounds_empty() {
    Region reg;
    Rect   b = reg.bounds();
    TEST_ASSERT_TRUE(b.empty());
}

void test_region_capacity_collapse() {
    /* Add far more disjoint rects than capacity. The region must stay bounded
     * (<= kMaxRects) by collapsing to its bounding box, and that box must still
     * cover every added rect -- the never-under-cover guarantee. */
    constexpr uint32_t kExtra = 8;
    Region             reg;
    for (uint32_t i = 0; i < Region::kMaxRects + kExtra; i++) {
        /* Disjoint 1x1 rects spaced apart so they don't merge. */
        reg.add(Rect{static_cast<int32_t>(i * 10), 0, static_cast<int32_t>(i * 10) + 1, 1});
    }
    TEST_ASSERT_LE(reg.count(), Region::kMaxRects); /* bounded -- safety valve worked */
    Rect b = reg.bounds();
    /* First rect at x=0; last rect right edge at ((N-1)*10)+1. Full span kept. */
    TEST_ASSERT_EQ(b.x0, 0);
    TEST_ASSERT_EQ(b.y0, 0);
    const int32_t last_x1 = static_cast<int32_t>((Region::kMaxRects + kExtra - 1) * 10) + 1;
    TEST_ASSERT_EQ(b.x1, last_x1);
    TEST_ASSERT_EQ(b.y1, 1);
}

}  // namespace test_gui_region

extern "C" void run_gui_region_tests() {
    TEST_SECTION("cinux::gui Region Tests (F13 §4b)");
    RUN_TEST(test_Rect::test_intersect_overlap);
    RUN_TEST(test_Rect::test_intersect_disjoint_degenerate);
    RUN_TEST(test_Rect::test_union_envelope);
    RUN_TEST(test_Rect::test_union_one_empty);
    RUN_TEST(test_Rect::test_translate);
    RUN_TEST(test_Rect::test_contains_point);
    RUN_TEST(test_Rect::test_contains_rect);
    RUN_TEST(test_Rect::test_empty_degenerate);
    RUN_TEST(test_gui_subtract::test_subtract_no_overlap);
    RUN_TEST(test_gui_subtract::test_subtract_hole_all_four);
    RUN_TEST(test_gui_subtract::test_subtract_covers_all);
    RUN_TEST(test_gui_subtract::test_subtract_edge_touch);
    RUN_TEST(test_gui_region::test_region_add_bounds);
    RUN_TEST(test_gui_region::test_region_add_ignores_degenerate);
    RUN_TEST(test_gui_region::test_region_intersect);
    RUN_TEST(test_gui_region::test_region_intersect_drops_outside);
    RUN_TEST(test_gui_region::test_region_translate);
    RUN_TEST(test_gui_region::test_region_subtract);
    RUN_TEST(test_gui_region::test_region_clear);
    RUN_TEST(test_gui_region::test_region_bounds_empty);
    RUN_TEST(test_gui_region::test_region_capacity_collapse);
    TEST_SUMMARY();
}

#else /* !CINUX_GUI */

extern "C" void run_gui_region_tests() {}

#endif /* CINUX_GUI */
