/**
 * @file host/gallery_dump_main.cpp
 * @brief One-frame gallery of EVERY widget -> PPM (模拟基准)
 *
 * Lays out one of each control (Label / Button / Slider / TextBox / CheckBox /
 * Radio+Group / Dropdown / Window) and dumps one PPM frame. This is the visual
 * baseline: any host rendering the same tree (sdl-host / fbdev-host QEMU /
 * host_cinux real Cinux) produces identical pixels -- core is host-neutral, the
 * Compositor is the single renderer.
 *
 * Standalone build only; not a ctest (eyeball the PPM).
 */

#include <cstdint>
#include <cstdio>

#include "event_payload.hpp"
#include "font.hpp"
#include "paint_list.hpp"
#include "ppm_writer.hpp"
#include "region.hpp"
#include "swraster.hpp"
#include "theme.hpp"
#include "widget.hpp"
#include "widget/button.hpp"
#include "widget/checkbox.hpp"
#include "widget/container.hpp"
#include "widget/dropdown.hpp"
#include "widget/label.hpp"
#include "widget/radio.hpp"
#include "widget/slider.hpp"
#include "widget/textbox.hpp"
#include "widget/window.hpp"

namespace {
using namespace cinux::gui;

PointerPayload pp(uint8_t kind, int32_t x, int32_t y) {
    PointerPayload p{};
    p.kind = kind;
    p.x    = x;
    p.y    = y;
    return p;
}
}  // namespace

int main() {
    constexpr uint32_t kW = 480;
    constexpr uint32_t kH = 320;

    PsfFont font;
    font.init();
    if (!font.ready()) {
        std::printf("font init failed\n");
        return 1;
    }
    Theme t = material_light();

    Container root;
    root.set_rect(0, 0, kW, kH);
    root.set_bg(t.background);

    /* Left column: one of each input control, manually laid out. */
    Label title;
    title.set_text("Cinux-GUI Gallery");
    title.set_color(t.on_surface);
    title.set_rect(16, 8, 300, 16);

    Label btn_l;
    btn_l.set_text("Button");
    btn_l.set_color(t.on_surface);
    btn_l.set_rect(16, 36, 80, 16);
    Button btn;
    btn.set_text("OK");
    btn.set_theme(&t);
    btn.set_rect(100, 32, 80, 24);

    Label slide_l;
    slide_l.set_text("Slider");
    slide_l.set_color(t.on_surface);
    slide_l.set_rect(16, 68, 80, 16);
    Slider slide;
    slide.set_range(100);
    slide.set_value(70);
    slide.set_theme(&t);
    slide.set_rect(100, 64, 150, 24);

    Label tb_l;
    tb_l.set_text("TextBox");
    tb_l.set_color(t.on_surface);
    tb_l.set_rect(16, 100, 80, 16);
    TextBox tb;
    tb.set_theme(&t);
    tb.set_rect(100, 96, 150, 24);

    Label cb_l;
    cb_l.set_text("CheckBox");
    cb_l.set_color(t.on_surface);
    cb_l.set_rect(16, 132, 80, 16);
    CheckBox cb;
    cb.set_theme(&t);
    cb.set_rect(100, 128, 24, 24);
    cb.on_pointer(pp(kPointerKindDown, 105, 133));  // toggle on, to show the checked state

    Label r_l;
    r_l.set_text("Radio");
    r_l.set_color(t.on_surface);
    r_l.set_rect(16, 164, 80, 16);
    Radio r1;
    r1.set_theme(&t);
    r1.set_rect(100, 160, 24, 24);
    Label r1_l;
    r1_l.set_text("Red");
    r1_l.set_color(t.on_surface);
    r1_l.set_rect(128, 160, 60, 24);
    Radio r2;
    r2.set_theme(&t);
    r2.set_rect(190, 160, 24, 24);
    Label r2_l;
    r2_l.set_text("Blue");
    r2_l.set_color(t.on_surface);
    r2_l.set_rect(218, 160, 60, 24);
    RadioGroup rg;
    rg.add(&r1);
    rg.add(&r2);
    r1.set_group(&rg);
    r2.set_group(&rg);
    r2.on_pointer(pp(kPointerKindDown, 195, 165));  // select Blue

    Label dd_l;
    dd_l.set_text("Dropdown");
    dd_l.set_color(t.on_surface);
    dd_l.set_rect(16, 196, 80, 16);
    Dropdown dd;
    dd.set_theme(&t);
    dd.set_option(0, "Apple");
    dd.set_option(1, "Banana");
    dd.set_option(2, "Cherry");
    dd.set_option_count(3);
    dd.set_rect(100, 192, 150, 3u * Dropdown::kRowH);
    dd.on_pointer(pp(kPointerKindDown, 105, 197));  // open, to show the list

    /* Right: a Window (title bar + close + resize grip + content). */
    Window win;
    win.set_title("Window");
    win.set_theme(&t);
    win.set_rect(300, 40, 160, 150);
    Label win_content;
    win_content.set_text("I am a\nwindow.");
    win_content.set_color(t.on_surface);
    win.set_content(&win_content);
    win.layout();

    root.add_child(&title);
    root.add_child(&btn_l);
    root.add_child(&btn);
    root.add_child(&slide_l);
    root.add_child(&slide);
    root.add_child(&tb_l);
    root.add_child(&tb);
    root.add_child(&cb_l);
    root.add_child(&cb);
    root.add_child(&r_l);
    root.add_child(&r1);
    root.add_child(&r1_l);
    root.add_child(&r2);
    root.add_child(&r2_l);
    root.add_child(&dd_l);
    root.add_child(&dd);
    root.add_child(&win);

    Desktop desktop;
    desktop.set_root(&root);

    uint8_t* buf = new uint8_t[kW * kH * 4u];
    Surface  staging{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    Region   dirty;
    desktop.render(staging, font, &dirty);

    const char* path = "gallery.ppm";
    if (!write_ppm(path, kW, kH, buf, staging.stride_bytes)) {
        std::printf("write_ppm(%s) failed\n", path);
        return 1;
    }
    /* Also write a BMP -- some viewers/IDEs preview BMP but not PPM. Same pixels
     * as the PPM, so it is an identical visual baseline for cross-host checks. */
    if (!write_bmp("gallery.bmp", kW, kH, buf, staging.stride_bytes)) {
        std::printf("write_bmp failed\n");
        return 1;
    }
    std::printf(
        "gallery-dump: OK -> %s + gallery.bmp (%ux%u: "
        "Label/Button/Slider/TextBox/CheckBox/Radio/Dropdown/Window)\n",
        path, kW, kH);
    delete[] buf;
    return 0;
}
