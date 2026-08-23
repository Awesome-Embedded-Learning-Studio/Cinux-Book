/**
 * @file test/test_widget.cpp
 * @brief Widget tree unit test -- hit-test + dispatch + flatten->pixels (P3-a)
 *
 * Drives the Widget/Desktop framework with a BoxWidget (paints a solid rect,
 * counts pointer deliveries). Covers: nested hit-test (child vs parent vs
 * outside), hidden-widget skip, dispatch delivery + miss, flatten->execute
 * pixel output, and clip containment (a child extending outside its parent's
 * rect is clipped at the parent boundary).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>  // memset

#include "compositor.hpp"  // execute
#include "event_payload.hpp"
#include "font.hpp"
#include "paint_list.hpp"
#include "region.hpp"
#include "swraster.hpp"
#include "widget.hpp"

using namespace cinux::gui;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            std::printf("FAIL: " __VA_ARGS__); \
            std::printf("\n");                 \
            return 1;                          \
        }                                      \
    } while (0)

namespace {

constexpr uint32_t kW = 320, kH = 240;

/* A minimal widget: paints its rect solid, counts on_pointer deliveries. */
class BoxWidget : public Widget {
public:
    uint32_t color;
    int      pointer_calls = 0;
    explicit BoxWidget(uint32_t c) : color(c) {}

    void on_pointer(const PointerPayload& /*p*/) override { pointer_calls++; }

protected:
    void paint_to_list(PaintList& list) const override {
        list.fill_rect(rect_.x0, rect_.y0, rect_.width(), rect_.height(), color);
    }
};

struct Stage {
    uint8_t* buf;
    Surface  s;
    Stage() : buf(new uint8_t[kW * kH * 4u]()), s{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888} {}
    ~Stage() { delete[] buf; }
    const uint32_t* px() const { return reinterpret_cast<const uint32_t*>(buf); }
};

PointerPayload pp(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p;
    std::memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}

}  // namespace

int main() {
    PsfFont font;
    font.init();
    CHECK(font.ready(), "font not ready");

    // 1. hit-test: nested child (on top of parent), outside miss, hidden skip
    {
        BoxWidget parent(0), child(0);
        parent.set_rect(0, 0, 100, 100);
        child.set_rect(40, 40, 20, 20);
        parent.add_child(&child);
        CHECK(parent.hit_test(50, 50) == &child, "point in child should hit child");
        CHECK(parent.hit_test(10, 10) == &parent, "point in parent-only should hit parent");
        CHECK(parent.hit_test(200, 200) == nullptr, "point outside should miss");
        child.set_visible(false);
        CHECK(parent.hit_test(50, 50) == &parent, "hidden child should be skipped");
    }

    // 2. dispatch + press capture (P3-d): down delivers + captures; a captured
    //    move delivers even outside the widget; up delivers + releases; a
    //    post-up move (no capture) is ignored
    {
        BoxWidget w(0);
        w.set_rect(0, 0, 100, 100);
        Desktop d;
        d.set_root(&w);
        d.dispatch_pointer(pp(kPointerKindDown, 50, 50));
        CHECK(w.pointer_calls == 1, "down delivers, calls=%d", w.pointer_calls);
        d.dispatch_pointer(pp(kPointerKindMove, 200, 200));  // outside, but captured
        CHECK(w.pointer_calls == 2, "captured move delivers, calls=%d", w.pointer_calls);
        d.dispatch_pointer(pp(kPointerKindUp, 50, 50));
        CHECK(w.pointer_calls == 3, "up delivers, calls=%d", w.pointer_calls);
        d.dispatch_pointer(pp(kPointerKindMove, 50, 50));  // no capture now
        CHECK(w.pointer_calls == 3, "post-up move ignored, calls=%d", w.pointer_calls);
    }

    // 3. flatten -> execute paints root + child (child on top)
    {
        Stage     st;
        BoxWidget root(0x00112233u);
        root.set_rect(0, 0, kW, kH);
        BoxWidget child(0x00FF0000u);
        child.set_rect(10, 10, 20, 20);
        root.add_child(&child);
        Desktop d;
        d.set_root(&root);
        Region dirty;
        d.render(st.s, font, &dirty);
        const uint32_t* px = st.px();
        CHECK(px[0] == 0x00112233u, "root pixel wrong 0x%08X", px[0]);
        CHECK(px[20 * kW + 20] == 0x00FF0000u, "child on top wrong 0x%08X", px[20 * kW + 20]);
    }

    // 4. clip containment: a child extending beyond its parent is clipped at
    //    the parent boundary (nothing paints outside the parent's rect)
    {
        Stage     st;  // zeroed buffer
        BoxWidget root(0);
        root.set_rect(0, 0, 50, 50);  // small root: only (0..50,0..50) is its clip
        BoxWidget overflow(0x00FF0000u);
        overflow.set_rect(40, 40, 100, 100);  // extends well past root
        root.add_child(&overflow);
        Desktop d;
        d.set_root(&root);
        Region dirty;
        d.render(st.s, font, &dirty);
        const uint32_t* px = st.px();
        CHECK(px[45 * kW + 45] == 0x00FF0000u, "inside parent+child should paint red");
        CHECK(px[80 * kW + 80] == 0u, "outside parent clip should be clipped");
    }

    std::printf("widget-test: OK (hit-test/dispatch/flatten-execute/clip)\n");
    return 0;
}
