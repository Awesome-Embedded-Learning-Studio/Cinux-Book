---
title: 06 · 接线:terminal-host 接真 shell
---

# 接线:terminal-host 接真 shell

### terminal-host:把真 shell 接上

理论讲完了,看实际怎么把 shell 接到这套 Widget 树上。就是 [`host/terminal_host_main.cpp`](../../../third_party/Cinux-GUI/host/terminal_host_main.cpp)——一个 SDL2 主程序,搭一棵 `WindowManager → Window → TerminalWidget`,spawn `/bin/sh` 在 PTY 里跑,主循环把键盘喂进 PTY、把 PTY 输出喂给 TerminalWidget。看组装部分:

```cpp
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
winw.set_content(&term);            // 终端挂进 Window 的 content 槽
winw.layout();                      // 重算 content rect(其实上面已设好,保险)
wm.add_window(&winw);               // Window 进 WM 的 Z 序

Desktop desktop;
desktop.set_root(&wm);              // WM 作桌面根
```

注意几个细节。**cols/rows 是按 content_rect 算的**,不是写死 80×25——窗口拉多大,终端就多少列。`set_rect` 的尺寸是 `cols * kGlyphW`(8)× `rows * kGlyphH`(16),保证终端 rect 正好被整数个 glyph 填满,不会有半个字。`set_content(&term)` 把 term 挂进 Window 的 content 槽 + 加进 Window 的 `children_`(这样 `flatten` 递归到它)。`desktop.set_root(&wm)` 把 WM 设成根,`Desktop::render` 就从这儿开始 flatten。

spawn shell 走 PTY,不是裸 pipe:

```cpp
setenv("TERM", "xterm-256color", 1);          // 让 ls --color / curses 发 SGR
int       in_fd  = -1;
int       out_fd = -1;
char*     argv[] = {const_cast<char*>("sh"), nullptr};
const int pid    = linux_spawn(nullptr, "/bin/sh", argv, &in_fd, &out_fd);
fcntl(out_fd, F_SETFL, O_NONBLOCK);           // 非阻塞 drain
```

`linux_spawn` 的实现在 [`host/posix_spawn.cpp`](../../../third_party/Cinux-GUI/host/posix_spawn.cpp#L16-L40),用的是 `forkpty`——它 fork 出一个子进程、把子的 stdio 挂到一个 PTY 对上、父进程拿到 **master fd**(双向:write 进 shell stdin、read 出 shell stdout)。`*stdin_fd = *stdout_fd = master`——签名跟 pipe 一样(两个 fd),内部其实是 PTY。为什么用 PTY 而不是裸 pipe?因为 PTY 给 shell 一个**控制终端**,行编辑(左箭头、Home、历史)和 curses 程序(vim/less)才能用。裸 pipe 够 ls/echo,但 curses 会烂。`setenv("TERM", "xterm-256color")` 是配套——shell 判断"要不要发彩色"不只看是不是 tty,还看 `$TERM` 是不是色采的;设成 `xterm-256color` 让 `ls --color` 发 256 色 SGR。

主循环把键盘和 PTY 接通:

```cpp
while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_MOUSEMOTION) {
            /* ... 包成 PointerPayload → wm.process_pointer ... */
        } else if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
            case SDLK_RETURN:     pty_write(in_fd, "\n", 1);            break;
            case SDLK_BACKSPACE:
            case SDLK_DELETE:    { const char d = 0x7f; pty_write(in_fd, &d, 1); } break;
            case SDLK_TAB:        pty_write(in_fd, "\t", 1);            break;
            case SDLK_UP:         pty_write(in_fd, "\x1b[A", 3);        break;
            case SDLK_DOWN:       pty_write(in_fd, "\x1b[B", 3);        break;
            case SDLK_RIGHT:      pty_write(in_fd, "\x1b[C", 3);        break;
            case SDLK_LEFT:       pty_write(in_fd, "\x1b[D", 3);        break;
            case SDLK_HOME:       pty_write(in_fd, "\x1b[H", 3);        break;
            case SDLK_END:        pty_write(in_fd, "\x1b[F", 3);        break;
            default: break;
            }
        } else if (e.type == SDL_TEXTINPUT) {
            pty_write(in_fd, e.text.text, strlen(e.text.text));
        }
    }

    /* drain shell 输出 → terminal,每帧封顶避免一次刷爆 */
    char     rbuf[1024];
    ssize_t  n;
    uint32_t read_total = 0u;
    while (read_total < 8192u && (n = read(out_fd, rbuf, sizeof(rbuf))) > 0) {
        term.write(rbuf, static_cast<uint32_t>(n));
        read_total += static_cast<uint32_t>(n);
    }

    Region dirty;
    desktop.render(staging, font, &dirty);
    if (dirty.count() > 0u) {                       // per-rect upload(只推脏区)
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
    SDL_Delay(16);                                  // ~60 fps
}
```

注意键盘走**双路**。`SDL_KEYDOWN` 只发控制键(Return / Backspace / Tab / Delete / 方向键 / Home / End)——SDL 对可打印字符发的是 `SDL_TEXTINPUT`(含 shift 组合、UTF-8 多字节)。两路互补不重复:控制键走 KEYDOWN 翻成对应字节序列(Backspace → `0x7f`、方向键 → `\x1b[A` 这种 ANSI 序列),可打印键走 TEXTINPUT 直接透传字节。这两路都不进 Widget 树——host 直接 `pty_write` 进 PTY master,根本没经过 `Desktop::dispatch_key`。

shell 的输出才进 Widget 树——host 每帧 `read(out_fd)` 抽一段、`term.write` 喂进 TerminalWidget。**输入不经控件、输出才进控件**——这是终端和普通输入控件的本质区别。普通文本框(后面的 TextBox)是你敲键盘、字符直接进控件状态;终端是你敲键盘、字符先去 shell、shell 决定回显什么、回显再进控件。这条分工让 TerminalWidget 没有 `on_key` override——它不需要。

还有一个细节是 read **每帧封顶 8192 字节**。shell 输出大爆发(比如 `ls /usr/lib` 列几千个文件、加载 bashrc)时,一次性 read 几万字节会让这一帧的 `put_char_` 跑几万次、卡住主循环。封顶 8192 让爆发分摊到多帧,画面保持响应(虽然输出慢一点冒出来)。这是 GUI 单线程 + 输入 SPSC 队列模型的一个典型妥协。

最后,host 拿到 dirty Region 后**逐 rect 上传纹理**:`SDL_UpdateTexture(tex, &sr, buf + r.y0*pitch + r.x0*4u, pitch)` 只更新那块矩形对应的纹理区域,而不是整张 texture。这正是保留模式脏区重绘在 host 层的落地——core 报"这几块变了",host 只把这几块推上 GPU。

