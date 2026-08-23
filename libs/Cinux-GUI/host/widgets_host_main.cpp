/**
 * @file host/widgets_host_main.cpp
 * @brief Widgets demo host -- Material widget tree, one PPM frame (P3-d)
 *
 * Builds a Desktop over an HBox of Label + Button + Slider (Material light),
 * renders one frame into a malloc staging buffer via the widget tree ->
 * PaintList -> execute path, verifies Material colours reach the pixels
 * (background, primary button body), and dumps PPM.
 *
 * Proves the P3 widget layer is end-to-end paintable (no pump/Host ABI here --
 * Desktop::render drives execute directly; the fbdev host wires the same tree
 * through the Host ABI for a real-device smoke).
 *
 * Standalone only.
 */

#include <cstdint>
#include <cstdio>

#include "compositor.hpp"  // execute (via Desktop::render)
#include "font.hpp"        // PsfFont
#include "paint_list.hpp"
#include "ppm_writer.hpp"
#include "region.hpp"
#include "swraster.hpp"
#include "theme.hpp"
#include "widget.hpp"
#include "widget/button.hpp"
#include "widget/container.hpp"
#include "widget/label.hpp"
#include "widget/slider.hpp"

namespace {

using namespace cinux::gui;

constexpr uint32_t kW = 320, kH = 240;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            std::printf("FAIL: " __VA_ARGS__); \
            std::printf("\n");                 \
            return 1;                          \
        }                                      \
    } while (0)

}  // namespace

int main() {
    PsfFont font;
    font.init();
    CHECK(font.ready(), "font not ready");
    Theme theme = material_light();

    HBox root;
    root.set_rect(0, 0, kW, kH);
    root.set_bg(theme.background);
    root.set_padding(16);
    root.set_spacing(16);

    Label title;
    title.set_text("Material");
    title.set_color(theme.on_surface);

    Button ok;
    ok.set_text("OK");
    ok.set_theme(&theme);

    Slider vol;
    vol.set_range(100);
    vol.set_value(60);
    vol.set_theme(&theme);

    root.add_child(&title);
    root.add_child(&ok);
    root.add_child(&vol);

    Desktop desktop;
    desktop.set_root(&root);

    uint8_t* buf = new uint8_t[kW * kH * 4u]();
    Surface  s{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};
    Region   dirty;
    desktop.render(s, font, &dirty);

    const uint32_t* px  = reinterpret_cast<const uint32_t*>(buf);
    const uint32_t  ppr = kW;

    /* corner is root background (children start at padding=16, so (0,0) is
     * outside any child and stays the HBox bg fill). */
    CHECK(px[0] == theme.background, "bg at origin 0x%08X", px[0]);

    /* HBox: pad=16, spacing=16, 3 children -> cw=288, child_w=(288-32)/3=85;
     * child1 (OK button) spans x=117..202; its body centre ~(159,120) is a
     * safe mid-row point (clear of the rounded corners) -> primary fill. */
    const uint32_t ok_px = px[120 * ppr + 159];
    CHECK(ok_px == theme.primary, "OK button primary 0x%08X", ok_px);

    const char* path = "widgets_frame.ppm";
    CHECK(write_ppm(path, kW, kH, buf, kW * 4u), "write_ppm(%s) failed", path);

    std::printf(
        "widgets-dump: OK -> %s (HBox: Label[Material] + Button[OK] + "
        "Slider@60, Material light)\n",
        path);
    delete[] buf;
    return 0;
}
