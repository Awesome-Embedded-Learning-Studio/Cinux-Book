/**
 * @file host/sdl_host_main.cpp
 * @brief SDL2 interactive host -- P7 widget demo (TextBox/Radio/Dropdown + keyboard)
 *
 * Click the Name box and type (keyboard routes via Desktop::dispatch_key), click
 * the radios (exclusive), open/close the dropdown. Opt-in (-DCINUX_HOST_SDL=ON);
 * host layer only, core never sees SDL.
 *
 * Run:  cmake -DCINUX_HOST_SDL=ON -S . -B build && cmake --build build -- sdl-host
 *       ./build/sdl-host     (WSL2: shows via WSLg)
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
#include "widget/container.hpp"
#include "widget/dropdown.hpp"
#include "widget/label.hpp"
#include "widget/radio.hpp"
#include "widget/slider.hpp"
#include "widget/textbox.hpp"

namespace {
using namespace cinux::gui;
}  // namespace

int main() {
    constexpr uint32_t kW = 320;
    constexpr uint32_t kH = 280;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win =
        SDL_CreateWindow("Cinux-GUI P7", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         static_cast<int>(kW) * 2, static_cast<int>(kH) * 2, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (ren == nullptr) {
        ren = SDL_CreateRenderer(win, -1, 0);
    }
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    SDL_RenderSetLogicalSize(ren, static_cast<int>(kW), static_cast<int>(kH));
    SDL_StartTextInput();  // P7-b: receive SDL_TEXTINPUT for the TextBox

    PsfFont font;
    font.init();
    Theme t = material_light();

    /* Manual layout (a VBox would divide equally; the Dropdown needs 3 rows). */
    Container root;
    root.set_rect(0, 0, kW, kH);
    root.set_bg(t.background);

    Label title;
    title.set_text("Cinux-GUI P7");
    title.set_color(t.on_surface);
    title.set_rect(16, 8, 200, 16);

    Label name_l;
    name_l.set_text("Name:");
    name_l.set_color(t.on_surface);
    name_l.set_rect(16, 32, 200, 16);
    TextBox name;
    name.set_theme(&t);
    name.set_rect(16, 50, 200, 24);

    Label color_l;
    color_l.set_text("Colour:");
    color_l.set_color(t.on_surface);
    color_l.set_rect(16, 82, 200, 16);
    Radio r1;
    r1.set_theme(&t);
    r1.set_rect(16, 100, 24, 24);
    Label red;
    red.set_text("Red");
    red.set_color(t.on_surface);
    red.set_rect(44, 100, 60, 24);
    Radio r2;
    r2.set_theme(&t);
    r2.set_rect(110, 100, 24, 24);
    Label blue;
    blue.set_text("Blue");
    blue.set_color(t.on_surface);
    blue.set_rect(138, 100, 60, 24);
    RadioGroup color_g;
    color_g.add(&r1);
    color_g.add(&r2);
    r1.set_group(&color_g);
    r2.set_group(&color_g);

    Label fruit_l;
    fruit_l.set_text("Fruit:");
    fruit_l.set_color(t.on_surface);
    fruit_l.set_rect(16, 134, 200, 16);
    Dropdown fruit;
    fruit.set_theme(&t);
    fruit.set_option(0, "Apple");
    fruit.set_option(1, "Banana");
    fruit.set_option(2, "Cherry");
    fruit.set_option_count(3);
    fruit.set_rect(16, 152, 150, 3u * Dropdown::kRowH);

    Label vol_l;
    vol_l.set_text("Volume:");
    vol_l.set_color(t.on_surface);
    vol_l.set_rect(16, 218, 200, 16);
    Slider vol;
    vol.set_range(100);
    vol.set_value(60);
    vol.set_theme(&t);
    vol.set_rect(16, 236, 200, 24);

    root.add_child(&title);
    root.add_child(&name_l);
    root.add_child(&name);
    root.add_child(&color_l);
    root.add_child(&r1);
    root.add_child(&red);
    root.add_child(&r2);
    root.add_child(&blue);
    root.add_child(&fruit_l);
    root.add_child(&fruit);
    root.add_child(&vol_l);
    root.add_child(&vol);

    Desktop desktop;
    desktop.set_root(&root);

    uint8_t* buf = new uint8_t[kW * kH * 4u];
    Surface  staging{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};

    std::printf("sdl-host P7: click Name + type, click radios, open dropdown. Ctrl-C exit.\n");
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
                desktop.dispatch_key(k);  // P7-b: TextBox backspace
            } else if (e.type == SDL_TEXTINPUT) {
                KeycodePayload k{};
                k.ascii = e.text.text[0];
                desktop.dispatch_key(k);  // P7-b: TextBox char input
            }
        }

        Region dirty;
        desktop.render(staging, font, &dirty);
        if (dirty.count() > 0u) {  // P5-f: upload only dirty rects
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
