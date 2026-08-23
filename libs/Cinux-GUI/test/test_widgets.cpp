/**
 * @file test/test_widgets.cpp
 * @brief Label/Button/Container + HBox/VBox -- basic widgets + layout (P3-c)
 *
 * Covers: Label bg+text glyph, Button rest/press colour + pointer state,
 * HBox/VBox linear layout geometry (equal share + gap), and a Desktop render of
 * an HBox of Material buttons (primary fill reaches the screen).
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
#include "widget/button.hpp"
#include "widget/container.hpp"
#include "widget/label.hpp"

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

/* Minimal widget for layout rect checks (no paint). */
class StubWidget : public Widget {};

}  // namespace

int main() {
    PsfFont font;
    font.init();
    CHECK(font.ready(), "font not ready");
    Theme t = material_light();

    // 1. Label paints bg + a text glyph in the top-left text region
    {
        Stage st;
        Label lbl;
        lbl.set_rect(0, 0, 100, 40);
        lbl.set_bg(0x00FF0000u);
        lbl.set_color(0x00FFFFFFu);
        lbl.set_text("Hi");
        Desktop d;
        d.set_root(&lbl);
        d.render(st.s, font, nullptr);
        const uint32_t* px = st.px();
        CHECK(px[0] == 0x00FF0000u, "label bg red");
        bool found_glyph = false;
        for (int32_t y = 4; y < 20 && !found_glyph; y++) {
            for (int32_t x = 4; x < 20; x++) {
                if (px[y * kW + x] == 0x00FFFFFFu) {
                    found_glyph = true;
                    break;
                }
            }
        }
        CHECK(found_glyph, "label text glyph not found");
    }

    // 2. Button rest = primary; down -> pressed + surface; up -> released
    {
        Stage  st;
        Button btn;
        btn.set_rect(0, 0, 100, 40);
        btn.set_text("OK");
        btn.set_theme(&t);
        Desktop d;
        d.set_root(&btn);
        d.render(st.s, font, nullptr);
        CHECK(st.px()[20 * kW + 50] == t.primary, "button rest centre = primary");
        d.dispatch_pointer(pp(kPointerKindDown, 50, 20));
        CHECK(btn.pressed(), "button pressed after down");
        d.render(st.s, font, nullptr);
        CHECK(st.px()[20 * kW + 50] == t.surface, "button press centre = surface");
        d.dispatch_pointer(pp(kPointerKindUp, 50, 20));
        CHECK(!btn.pressed(), "button released after up");
    }

    // 3. HBox layout: 3 children equal width + 10px gap
    {
        HBox hbox;
        hbox.set_rect(0, 0, 300, 40);
        hbox.set_spacing(10);
        StubWidget a, b, c;
        hbox.add_child(&a);
        hbox.add_child(&b);
        hbox.add_child(&c);
        hbox.layout();
        CHECK(a.rect().x0 == 0 && a.rect().width() == 93, "hbox child a");
        CHECK(b.rect().x0 == 103 && b.rect().width() == 93, "hbox child b");
        CHECK(c.rect().x0 == 206 && c.rect().width() == 94,
              "hbox child c (P5-d: last child gets the remainder)");
    }

    // 4. VBox layout: 2 children equal height + 10px gap
    {
        VBox vbox;
        vbox.set_rect(0, 0, 40, 300);
        vbox.set_spacing(10);
        StubWidget a, b;
        vbox.add_child(&a);
        vbox.add_child(&b);
        vbox.layout();
        CHECK(a.rect().y0 == 0 && a.rect().height() == 145, "vbox child a");
        CHECK(b.rect().y0 == 155 && b.rect().height() == 145, "vbox child b");
    }

    // 5. Desktop render: HBox(bg) with 2 buttons -> both primary bodies painted
    {
        Stage st;
        HBox  hbox;
        hbox.set_rect(0, 0, kW, kH);
        hbox.set_bg(t.background);
        Button b1;
        b1.set_text("A");
        b1.set_theme(&t);
        Button b2;
        b2.set_text("B");
        b2.set_theme(&t);
        hbox.add_child(&b1);
        hbox.add_child(&b2);
        Desktop d;
        d.set_root(&hbox);
        d.render(st.s, font, nullptr);
        const uint32_t* px = st.px();
        // buttons share width equally: b1 [0,160), b2 [160,320); body centres
        CHECK(px[120 * kW + 80] == t.primary, "hbox b1 body primary");
        CHECK(px[120 * kW + 240] == t.primary, "hbox b2 body primary");
    }

    std::printf("widgets-test: OK (label/button-rest-press/hbox/vbox/desktop)\n");
    return 0;
}
