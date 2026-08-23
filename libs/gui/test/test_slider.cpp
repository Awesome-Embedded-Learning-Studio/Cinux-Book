/**
 * @file test/test_slider.cpp
 * @brief Slider widget -- drag-to-value + clamp + paint (P3-d)
 *
 * Uses Desktop press capture: a down on the slider captures it, so move events
 * keep changing the value even past the thumb; up releases; a post-up move is
 * ignored. Also checks over/under-range clamp and that the thumb paints primary
 * at the value position.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "compositor.hpp"
#include "event_payload.hpp"
#include "font.hpp"
#include "paint_list.hpp"
#include "region.hpp"
#include "swraster.hpp"
#include "theme.hpp"
#include "widget.hpp"
#include "widget/slider.hpp"

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
    Theme t = material_light();

    // 1. drag mid -> 50; release keeps value; post-release move ignored
    {
        Slider sl;
        sl.set_rect(0, 0, 200, 40);
        sl.set_range(100);
        sl.set_theme(&t);
        Desktop d;
        d.set_root(&sl);
        CHECK(sl.value() == 0, "initial value 0");
        d.dispatch_pointer(pp(kPointerKindDown, 4, 20));
        CHECK(sl.value() == 0, "down at left = 0");
        d.dispatch_pointer(pp(kPointerKindMove, 100, 20));
        CHECK(sl.value() == 50, "drag to mid = 50, got %d", sl.value());
        d.dispatch_pointer(pp(kPointerKindUp, 100, 20));
        CHECK(sl.value() == 50, "release keeps 50");
        d.dispatch_pointer(pp(kPointerKindMove, 250, 20));  // no capture
        CHECK(sl.value() == 50, "post-release move ignored, got %d", sl.value());
    }

    // 2. clamp to [0, range] when dragged past either end
    {
        Slider sl;
        sl.set_rect(0, 0, 200, 40);
        sl.set_range(100);
        sl.set_theme(&t);
        Desktop d;
        d.set_root(&sl);
        d.dispatch_pointer(pp(kPointerKindDown, 4, 20));
        d.dispatch_pointer(pp(kPointerKindMove, 300, 20));  // beyond right
        CHECK(sl.value() == 100, "clamp to 100, got %d", sl.value());
        d.dispatch_pointer(pp(kPointerKindMove, -50, 20));  // beyond left
        CHECK(sl.value() == 0, "clamp to 0, got %d", sl.value());
    }

    // 3. paint: thumb is primary at the value position (value 50 -> x=100)
    {
        Stage  st;
        Slider sl;
        sl.set_rect(0, 0, 200, 40);
        sl.set_range(100);
        sl.set_value(50);
        sl.set_theme(&t);
        Desktop d;
        d.set_root(&sl);
        d.render(st.s, font, nullptr);
        const uint32_t* px = st.px();
        CHECK(px[20 * kW + 100] == t.primary, "thumb primary at value 50");
    }

    std::printf("slider-test: OK (drag/clamp/paint)\n");
    return 0;
}
