/**
 * @file host/terminal_host_main.cpp
 * @brief SDL2 interactive terminal host -- a real shell in a window (P4-d)
 *
 * Opens an SDL2 window, hosts a WindowManager with one Window whose content is
 * a TerminalWidget, spawns /bin/sh under a PTY (linux_spawn / forkpty), and
 * wires keyboard -> PTY and PTY -> TerminalWidget::write each frame. The result
 * is a real interactive shell rendered entirely by the core widget stack.
 *
 * Run:  cmake -DCINUX_HOST_TERMINAL=ON -S . -B build && cmake --build build -- terminal-host
 *       ./build/terminal-host     (WSL2: shows via WSLg; type shell commands)
 *
 * HOST layer (SDL2 + PTY); core stays host-neutral.
 */

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>  // setenv (P5-e: TERM for ls --color)

#include "event_payload.hpp"
#include "font.hpp"
#include "posix_spawn.hpp"
#include "region.hpp"
#include "swraster.hpp"
#include "theme.hpp"
#include "widget.hpp"
#include "widget/terminal.hpp"
#include "widget/window.hpp"
#include "widget/window_manager.hpp"

namespace {
using namespace cinux::gui;

void pty_write(int fd, const void* p, size_t n) {
    ssize_t r = write(fd, p, n);  // best-effort; ignore EAGAIN/retries
    (void)r;
}
}  // namespace

int main() {
    constexpr uint32_t kW = 720;
    constexpr uint32_t kH = 440;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win =
        SDL_CreateWindow("Cinux-GUI terminal", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         static_cast<int>(kW), static_cast<int>(kH), 0);
    if (win == nullptr) {
        std::printf("window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (ren == nullptr) {
        ren = SDL_CreateRenderer(win, -1, 0);
    }
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    SDL_RenderSetLogicalSize(ren, static_cast<int>(kW), static_cast<int>(kH));
    SDL_StartTextInput();  // receive SDL_TEXTINPUT for printable keys

    PsfFont font;
    font.init();
    if (!font.ready()) {
        std::printf("font init failed\n");
        return 1;
    }
    Theme t = material_dark();

    WindowManager wm;
    wm.set_rect(0, 0, kW, kH);
    wm.set_theme(&t);

    Window winw;
    winw.set_title("Terminal");
    winw.set_theme(&t);
    winw.set_rect(8, 8, kW - 16, kH - 16);

    const Rect     cr   = winw.content_rect();
    const uint32_t cols = cr.width() / TerminalWidget::kGlyphW;
    const uint32_t rows = cr.height() / TerminalWidget::kGlyphH;

    TerminalWidget term;
    term.set_theme(&t);
    term.set_cols_rows(cols, rows);
    term.set_rect(cr.x0, cr.y0, cols * TerminalWidget::kGlyphW, rows * TerminalWidget::kGlyphH);
    winw.set_content(&term);
    winw.layout();
    wm.add_window(&winw);

    Desktop desktop;
    desktop.set_root(&wm);

    // spawn /bin/sh under a PTY. Set TERM so ls --color / curses emit SGR (the
    // PTY is a tty, but programs also gate colour on $TERM being colour-capable).
    setenv("TERM", "xterm-256color", 1);
    int       in_fd  = -1;
    int       out_fd = -1;
    char*     argv[] = {const_cast<char*>("sh"), nullptr};
    const int pid    = linux_spawn(nullptr, "/bin/sh", argv, &in_fd, &out_fd);
    if (pid <= 0) {
        std::printf("spawn /bin/sh failed\n");
        return 1;
    }
    fcntl(out_fd, F_SETFL, O_NONBLOCK);  // non-blocking drain each frame

    uint8_t* buf = new uint8_t[kW * kH * 4u];
    Surface  staging{buf, kW, kH, kW * 4u, PixelFormat::kXrgb8888};

    std::printf("terminal-host: shell pid %d -- type commands. Close window or Ctrl-C to exit.\n",
                pid);
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
                wm.process_pointer(p);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                PointerPayload p{};
                p.kind    = kPointerKindDown;
                p.x       = e.button.x;
                p.y       = e.button.y;
                p.buttons = 1u;
                wm.process_pointer(p);
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                PointerPayload p{};
                p.kind    = kPointerKindUp;
                p.x       = e.button.x;
                p.y       = e.button.y;
                p.buttons = 0u;
                wm.process_pointer(p);
            } else if (e.type == SDL_KEYDOWN) {
                /* Control/edit keys send their terminal byte sequence directly
                 * (printables arrive via SDL_TEXTINPUT). Backspace = DEL (0x7f),
                 * the PTY's VERASE; arrows / Home / End send ANSI cursor keys so
                 * shell line-editing + history work. */
                switch (e.key.keysym.sym) {
                    case SDLK_RETURN:
                        pty_write(in_fd, "\n", 1);
                        break;
                    case SDLK_BACKSPACE:
                    case SDLK_DELETE: {
                        const char d = 0x7f;
                        pty_write(in_fd, &d, 1);
                        break;
                    }
                    case SDLK_TAB:
                        pty_write(in_fd, "\t", 1);
                        break;
                    case SDLK_UP:
                        pty_write(in_fd, "\x1b[A", 3);
                        break;
                    case SDLK_DOWN:
                        pty_write(in_fd, "\x1b[B", 3);
                        break;
                    case SDLK_RIGHT:
                        pty_write(in_fd, "\x1b[C", 3);
                        break;
                    case SDLK_LEFT:
                        pty_write(in_fd, "\x1b[D", 3);
                        break;
                    case SDLK_HOME:
                        pty_write(in_fd, "\x1b[H", 3);
                        break;
                    case SDLK_END:
                        pty_write(in_fd, "\x1b[F", 3);
                        break;
                    default:
                        break;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                pty_write(in_fd, e.text.text, strlen(e.text.text));
            }
        }

        // drain shell output -> terminal, capped per frame so a large burst
        // (bashrc / `ls /usr/lib`) spreads across frames instead of stalling one.
        char     rbuf[1024];
        ssize_t  n;
        uint32_t read_total = 0u;
        while (read_total < 8192u && (n = read(out_fd, rbuf, sizeof(rbuf))) > 0) {
            term.write(rbuf, static_cast<uint32_t>(n));
            read_total += static_cast<uint32_t>(n);
        }

        Region dirty;
        desktop.render(staging, font, &dirty);
        if (dirty.count() > 0u) {  // P5-f: upload only the dirty rects
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
        SDL_Delay(16);  // ~60 fps
    }

    close(in_fd);  // SIGHUP to the shell
    delete[] buf;
    SDL_StopTextInput();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
