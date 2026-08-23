/**
 * @file host/demo_host_main.cpp
 * @brief Rich Material widget demo -- one PPM frame to eyeball (P3)
 *
 * A VBox of [title, HBox of 3 buttons, label, slider, HBox of 2 buttons] in
 * Material light -- shows off the widget tree, nested HBox/VBox layout, and
 * rounded/coloured controls. NOT a test (no assertions); dump and look.
 *
 * Standalone only.
 */

#include <cstdint>
#include <cstdio>

#include "compositor.hpp"
#include "font.hpp"
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
}  // namespace

int main() {
    PsfFont font;
    font.init();
    if (!font.ready()) {
        std::printf("demo: font init failed\n");
        return 1;
    }
    Theme t = material_light();

    VBox root;
    root.set_rect(0, 0, 320, 240);
    root.set_bg(t.background);
    root.set_padding(16);
    root.set_spacing(8);

    Label title;
    title.set_text("Cinux-GUI");
    title.set_color(t.on_surface);

    Label vol_label;
    vol_label.set_text("Volume");
    vol_label.set_color(t.on_surface);

    HBox actions;
    actions.set_spacing(8);
    Button bnew;
    bnew.set_text("New");
    bnew.set_theme(&t);
    Button bopen;
    bopen.set_text("Open");
    bopen.set_theme(&t);
    Button bsave;
    bsave.set_text("Save");
    bsave.set_theme(&t);
    actions.add_child(&bnew);
    actions.add_child(&bopen);
    actions.add_child(&bsave);

    Slider vol;
    vol.set_range(100);
    vol.set_value(60);
    vol.set_theme(&t);

    HBox footer;
    footer.set_spacing(8);
    Button ok;
    ok.set_text("OK");
    ok.set_theme(&t);
    Button cancel;
    cancel.set_text("Cancel");
    cancel.set_theme(&t);
    footer.add_child(&ok);
    footer.add_child(&cancel);

    root.add_child(&title);
    root.add_child(&actions);
    root.add_child(&vol_label);
    root.add_child(&vol);
    root.add_child(&footer);

    Desktop desktop;
    desktop.set_root(&root);

    uint8_t* buf = new uint8_t[320 * 240 * 4u]();
    Surface  s{buf, 320, 240, 320u * 4u, PixelFormat::kXrgb8888};
    Region   dirty;
    desktop.render(s, font, &dirty);

    const char* path = "demo.ppm";
    write_ppm(path, 320, 240, buf, 320u * 4u);
    std::printf(
        "demo-dump: OK -> %s (VBox: title + [New/Open/Save] + Volume + "
        "slider@60 + [OK/Cancel], Material light)\n",
        path);
    delete[] buf;
    return 0;
}
