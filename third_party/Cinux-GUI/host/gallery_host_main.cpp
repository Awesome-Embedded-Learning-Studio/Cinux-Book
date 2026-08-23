/**
 * @file host/gallery_host_main.cpp
 * @brief Interactive gallery -- one of every widget, live (SDL2)
 *
 * The runnable counterpart to gallery-dump: the same widget tree, but driven by
 * real mouse + keyboard. Click the CheckBox / Radios / Dropdown / Button, drag
 * the Slider, click the TextBox and type, drag/close/resize the Window. Opt-in
 * (-DCINUX_HOST_GALLERY=ON); host layer only, core never sees SDL.
 *
 * Run:  cmake -DCINUX_HOST_GALLERY=ON -S . -B build && cmake --build build -- gallery-host
 *       ./build/gallery-host     (WSL2: shows via WSLg)
 */

#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdio>

#include "event_payload.hpp"
#include "font.hpp"
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
}  // namespace

int main() {
    constexpr uint32_t kW = 480;
    constexpr uint32_t kH = 320;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win =
        SDL_CreateWindow("Cinux-GUI Gallery", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         static_cast<int>(kW) * 2, static_cast<int>(kH) * 2, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (ren == nullptr) {
        ren = SDL_CreateRenderer(win, -1, 0);
    }
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    SDL_RenderSetLogicalSize(ren, static_cast<int>(kW), static_cast<int>(kH));
    SDL_StartTextInput();  // TextBox keyboard input

    PsfFont font;
    font.init();
    Theme t = material_light();

    /* Same manual layout as gallery-dump (VBox can't give Dropdown 3 rows). */
    Container root;
    root.set_rect(0, 0, kW, kH);
    root.set_bg(t.background);

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

    Window winw;
    winw.set_title("Window");
    winw.set_theme(&t);
    winw.set_rect(300, 40, 160, 150);
    Label win_content;
    win_content.set_text("I am a\nwindow.");
    win_content.set_color(t.on_surface);
    winw.set_content(&win_content);
    winw.layout();

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
    root.add_child(&winw);

    Desktop desktop;
    desktop.set_root(&root);

    uint8_t* buf = new uint8_t[kW * kH * 4u];
    Surface  staging{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};

    std::printf(
        "gallery-host: click widgets, type in TextBox, drag/close/resize the Window. Ctrl-C "
        "exit.\n");
    std::fflush(stdout);

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_MOUSEMOTION) {
                PointerPayload p{};
                p.kind    = kPointerKindMove;
                p.x       = e.motion.x;
                p.y       = e.motion.y;
                p.buttons = (e.motion.state & SDL_BUTTON_LMASK) ? 1u : 0u;
                desktop.dispatch_pointer(p);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                PointerPayload p{};
                p.kind    = kPointerKindDown;
                p.x       = e.button.x;
                p.y       = e.button.y;
                p.buttons = 1u;
                desktop.dispatch_pointer(p);
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                PointerPayload p{};
                p.kind    = kPointerKindUp;
                p.x       = e.button.x;
                p.y       = e.button.y;
                p.buttons = 0u;
                desktop.dispatch_pointer(p);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE) {
                KeycodePayload k{};
                k.ascii = '\b';
                desktop.dispatch_key(k);
            } else if (e.type == SDL_TEXTINPUT) {
                KeycodePayload k{};
                k.ascii = e.text.text[0];
                desktop.dispatch_key(k);
            }
        }

        Region dirty;
        desktop.render(staging, font, &dirty);
        if (dirty.count() > 0u) {
            const uint32_t pitch = kW * 4u;
            for (uint32_t i = 0u; i < dirty.count(); ++i) {
                const Rect&    r = dirty.rects()[i];
                const SDL_Rect sr{r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
                SDL_UpdateTexture(tex, &sr, buf + r.y0 * pitch + r.x0 * 4u, pitch);
            }
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(16);
    }

    delete[] buf;
    SDL_StopTextInput();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
