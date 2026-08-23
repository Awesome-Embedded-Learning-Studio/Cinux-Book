# P4-d — Terminal IO + PTY spawn（真 shell 跑起来）

> P4 第四批。把 P4-c 的 TerminalWidget 接上**真 shell** —— CinuxOS terminal 经内核 `Pipe` + `TaskBuilder`/`launch_user_program` spawn shell；本仓 host 层用 POSIX `forkpty` + `execvp` 重建，core 仍 host-neutral（core 不做 IO）。

## 背景

P4-c 立了 TerminalWidget 显示层（write 字节即显示），但字节来源是注桩。真终端要 spawn `/bin/sh`，键盘喂进去、输出显示出来。这批接通 IO，且首实装 spawn 能力（host 层）。

## 目标

terminal-host（SDL2 窗）跑真 `/bin/sh`：键盘 → PTY → shell → PTY → terminal.write → 渲染。spawn 用 PTY（curses/行编辑可用），签名跟 `HostDesktop::spawn` 对齐（便于将来填 ABI 表），**Host ABI 零改动**。

## 设计

**关键认知**：sdl-host 是 standalone harness —— **不经 Host ABI 表，直接用 core**（Desktop/Surface/render）。terminal-host 同模式，直接调 core 的 WM/Window/TerminalWidget，`spawn` 作 host 侧 `linux_spawn` 函数。

- **host/posix_spawn**：`linux_spawn(ctx, path, argv, stdin_fd, stdout_fd)` = `forkpty` + `execvp`。parent 拿 PTY **master fd**（双向：write→shell stdin，read←shell stdout）。`*stdin_fd = *stdout_fd = master`（pipe 签名不变，内部 PTY）。返回 pid。签名跟 `HostDesktop::spawn` 一致 → 将来 host 填表直接用。
- **host/terminal_host_main**：SDL2 窗 720×440 + `WindowManager` + `Window`（content=`TerminalWidget`，cols/rows 据 content 算）+ 主循环：
  - SDL 鼠标 → `wm.process_pointer`（拖窗/raise/关闭）。
  - SDL `TEXTINPUT`（可打印）+ `KEYDOWN`（Return/Backspace/Tab/Delete）→ `pty_write(in_fd)`。
  - 每帧非阻塞 `read(out_fd)` → `terminal.write(buf, n)`（shell 输出上屏）。
  - `desktop.render(staging, font, &dirty)`（WM 作 root，flatten 画 bg+windows+cursor）。
  - `SDL_UpdateTexture` + present，~60fps。

## 决策

1. **standalone harness 直接用 core，不经 ABI 表**：跟 sdl-host 一致。Host ABI 表是给**外部 host**（CinuxOS host_cinux）经 `GuiCore::pump` 用的；standalone harness 直接调 core API 更简单。`linux_spawn` 签名仍对齐 ABI，不堵将来填表。
2. **PTY 而非裸 pipe**：PTY 给 controlling tty，shell 行编辑 / curses 可用（vim/less 能跑）。pipe 够 ls/echo 但 curses 烂。master fd 双向填进 spawn 的 stdin/stdout —— pipe 签名不变，**Host ABI 零改动**（P4 决策②承诺）。
3. **core 零改动**：TerminalWidget.write（P4-c）就是 IO 接口；host 喂字节。core 对 PTY/spawn 无知。
4. **键盘双路**：`TEXTINPUT`（可打印，含 shift/UTF-8）+ `KEYDOWN`（控制键 Return/Backspace/Tab/Delete）。SDL 对可打印键发 TEXTINPUT、对控制键只发 KEYDOWN，两路互补不重复。

## 陷阱

- **`write` 的 `__wur`**：glibc `write` 是 `warn_unused_result`，`-Wall -Wextra` 下裸 `write(...)` 报警。封 `pty_write`（`ssize_t r=write; (void)r;`）抑制。
- **PTY 默认 ECHO + ICANON**：键盘 write master → slave(sh) 回 echo → master read 回来 → terminal 显示。即"输入即时回显"是 PTY 行为，不是 host 逻辑，正常。
- **`forkpty` 链 `-lutil`**：CMake `target_link_libraries(... util)`（bare `util` → `-lutil`）。
- **non-blocking read + 子进程 EOF**：posix-spawn-test 用 O_NONBLOCK + 200ms 轮询读，子进程 exit 后 read 返回 0（EOF）。ctest 稳定（echo 快）。
- **`scene::Window` vs `widget::Window` 命名冲突**：terminal-host 同时拉 `compositor.hpp`（→ `scene.hpp` 的 P2 `struct Window` POD）与 `widget/window.hpp`（P4 `class Window` Widget）→ 重定义。绕过：terminal-host **不 include compositor.hpp**（它只调 `desktop.render`，声明在 widget.hpp；execute 在 widget.cpp 内部）。P4-e 退役 Scene 后彻底解。

## 验证

- 新 `test_posix_spawn.cpp`：`/bin/sh -c "echo hi"` → forkpty + read master → 断言收到 "hi"；`in_fd == out_fd`（同一 master）。**ctest 15/15 绿**（含此 host 测）。
- **ASAN 干净**（posix-spawn-test：主进程 ASAN，子进程 exec 后非 ASAN）。
- **terminal-host 编译 + 链接通过**（SDL2 + util + core，opt-in `-DCINUX_HOST_TERMINAL=ON`）。
- **真 shell 手动冒烟**：`./build/terminal-host`（WSLg 开窗，键盘打 shell 命令，输出实时上屏，标题栏可拖/关闭）。手动眼检，非 CI。
- Host ABI 零改动（core/host.hpp 没碰）；core 仍 host-neutral。

## CinuxOS 侧

零改动。CinuxOS `kernel/gui/{terminal,gui_init}.cpp`（Pipe + TaskBuilder spawn）暂留对照。

## 下一步

P4-e fbdev host 切控件树（替代 P2 Scene 路径）+ `core/scene.*` / `compose(Scene)` 退役 + desktop icon 数据。QEMU 真 evdev → WM 全链路冒烟。
