---
title: Lab 031 · 原生终端应用:让窗口能打字
---

# Lab 031 · 原生终端应用:让窗口能打字

> 这个实验对应主书 [031 · 原生终端应用](../../book/09-gui/031-gui-native-app.md)。我们不在 lab 里贴完整答案代码——你要自己把"一个能打字的窗口"一层层搭起来。这里只给目标、约束、验证手段和排错方向。

## 实验目标

在 030 的"空窗口"基础上,做出内核里**第一个跑在窗口里的应用**:一个 80×25 的终端窗口。开机进 GUI,点中它拿到焦点,敲键盘,字符带着光标出现在窗口里,写满一行自动换行、写满一屏向上滚动。这一章只做**本地回显**(你敲什么画什么);让它真正跑 shell,是 [Lab 031b](lab-031b-gui-pipe.md) 的事。

具体要点亮四件事:`Window` 变成多态基类、键盘事件第一次被窗口消费、`Terminal` 这个控件本身、以及每帧把它光栅化上屏。

## 前置条件

- 跑通 030:`Window` / `WindowManager` / `Canvas` / `EventQueue` / PS/2 鼠标 / PIT 滴答里的 `gui_tick_callback`。
- 跑通 014:PS/2 键盘,键盘事件已经能经 `irq1_handler` 的双路分发塞进 `Mouse::event_queue()`(这条队列名字带 Mouse,实质是全局 GUI 事件队列)。
- 029 的 `Canvas` 离屏构造 `init(w, h)`(窗口双缓冲要用)。

## 任务分解

按依赖顺序,分七块做。

### 任务 1:让 Window 变成多态基类

改 `kernel/gui/window.hpp` 的 `Window`。

- 给它**虚析构** `virtual ~Window() = default;`,否则以后通过基类指针 delete 派生类会泄漏。
- 加两个虚函数,基类给空实现(吃掉参数):`virtual void on_key(KeyEvent& ev) { (void)ev; }` 和 `virtual void on_paint(cinux::drivers::Canvas& canvas) { (void)canvas; }`。
- 想清楚为什么必须虚化:窗口管理器手里只有 `Window*`,它要在**不认识 Terminal 这个具体类型**的前提下把键盘事件派发下去,靠的就是虚函数表。没有这套虚机制,WM 就得 `switch` 判断窗口类型,每加一种窗口都得回来改 WM。

### 任务 2:WindowManager 路由键盘 + 接收外部窗口

改 `kernel/gui/window_manager.{hpp,cpp}`。

- **`handle_key` 实化**:030 它是空桩(`(void)ev;`),现在写成 `if (focused_ != nullptr) focused_->on_key(ev.key);`。注意它**不做命中测试**——键盘只给 `focused_`,鼠标才 `hit_test`。`ev.key` 是 `Event` 联合里的 `KeyEvent`,按引用透传。
- **新增 `add_window(Window* win)`**:接收外部已经 `new` 好的窗口(所有权转移),区别于内部自己 `new Window` 的 `create()`。为什么需要它?因为 `Terminal` 是 `Window` 的派生类,WM 没法替你 `new` 一个派生类。`add_window` 的后半段(画标题栏、画内容、`count_++`、`update_focus`)和 `create` 共用。
- 想清楚 `focused_` 怎么来:`update_focus()` 把最顶(`windows_[count_-1]`)窗口设成焦点。`raise`/`add_window`/`destroy` 都会调它重算。

### 任务 3:Terminal 控件骨架

新建 `kernel/gui/terminal.{hpp,cpp}`(`#ifdef CINUX_GUI` 守卫),命名空间 `cinux::gui`。

- **`TerminalCell`**:`{ char ch=' '; uint32_t fg=0x00FFFFFF; uint32_t bg=0x00000000; }`(默认白字黑底空格)。
- **`Terminal : public Window`**:常量 `COLS=80`、`ROWS=25`;私有成员 `TerminalCell screen_[ROWS][COLS]`、`cursor_x_`、`cursor_y_`、`fg_`、`bg_`、`cursor_visible_`、`font_`(PSFFont*)、以及两个先留空的管道指针 `stdin_pipe_`/`stdout_pipe_`。
- **构造**:`Terminal(uint32_t x, uint32_t y, const char* title)`,把基类 Window 初始化为 `title + (x,y) + COLS*8 宽 + ROWS*16 高`,再把 `screen_` 全部置 `TerminalCell{}`。窗口像素尺寸 = 640×400,因为 PSF 字体每字符 8×16。
- 不可拷贝(`= delete`)。

### 任务 4:字符怎么落屏

实现 `write(const char* str, uint64_t len)` 和它调用的几个 helper。`write` 逐字节分发:`'\n'`→`newline()`、`'\r'`→`cursor_x_=0`、`'\b'`→`backspace()`、`'\t'`→`tab()`、其余→`process_char()`;ESC(`\033`)交给任务 5 的 `handle_ansi`。

- **`process_char`**:只认可打印 ASCII(`0x20..0x7E`,其余直接丢);把字符写进 `screen_[cursor_y_][cursor_x_]`,`cursor_x_++`;到 `COLS` 就归零并 `newline()`。
- **`newline`**:`cursor_x_=0; cursor_y_++;` 若 `cursor_y_ >= ROWS`,**先把 `cursor_y_` 钳到 `ROWS-1` 再 `scroll_up()`**——顺序不能反,否则会访问 `screen_[ROWS][...]` 越界。
- **`scroll_up`**:把第 1..ROWS-1 行整体上移一行、顶行(第 0 行)被覆盖丢弃,最后一行清空。注意:本 lab **不做 scrollback**,滚出屏幕的内容永久丢失,这是有意的简化。
- **`backspace`**:行内退一格;行首则退到上一行末尾。
- **`tab`**:跳到下一个 8 列制表位 `(cursor_x_/8+1)*8`,到边界钳到 `COLS-1`。

### 任务 5:极简 ANSI

实现 `is_escape(char)`(就是 `ch == '\033'`)和 `handle_ansi(const char* str, uint64_t len, uint64_t& pos)`。

- 期望 `ESC [`(CSI);不是 CSI 就 `pos++` 跳过 ESC 返回。
- 跳过 `ESC [` 后,循环收集数字(累加成单个 `param`)和 `;`,遇到字母(final byte)派发:`'J'` 且 `param==2`→`clear()`;`'H'`→`cursor_x_=0, cursor_y_=0`;`'K'`→把当前行 `cursor_x_..COLS-1` 清空;`'m'`(SGR)→直接忽略;default→return。
- **`pos` 用引用传出**,`write` 的循环靠它跳过整段转义序列。关键边界:只解析**单个**数值参数,`'H'` 无条件归零(所以 `ESC[10;20H` 在本终端等价于 `ESC[H`),`'m'` 被明确忽略。别声称支持颜色或光标定位。

### 任务 6:render_to_canvas 光栅化

实现 `render_to_canvas()` 和 `on_paint(Canvas&)`(后者形参忽略,体内只 `if (font_) render_to_canvas();`)。

- 开头 `if (font_ == nullptr) return;`——**这条守卫是"开窗空白"的命门**,见常见故障。
- 取基类画布 `auto& cvs = canvas();`(注意不是 `on_paint` 传进来的那个形参),`gw = font_->width()`、`gh = font_->height()`。
- 遍历每个 cell:`px = col*gw`、`py = TITLE_BAR_HEIGHT + row*gh`(内容必须画在标题栏下方,`TITLE_BAR_HEIGHT` 是基类定的 20);先 `draw_rect(px,py,gw,gh,cell.bg)`,若 `cell.ch > ' '` 就取 glyph 逐位光栅化:`(g[gr] >> (7-gc)) & 1` 为 1 则 `draw_pixel(px+gc, py+gr, cell.fg)`。
- 光标用反色块:`cursor_visible_` 时,在光标格 `draw_rect(cx,cy,gw,gh, cc.fg)` 铺满,再用 `cc.bg` 重画该格字符——天然反相、透出底下字符。

### 任务 7:gui_start 点火 + tick 刷新

改 `kernel/gui/gui_init.cpp`。

- **`gui_start()` 返回类型从 `void` 改成 `Terminal*`**:`Mouse::init()` → `set_screen_bounds` → 算终端尺寸(640×400)并居中 → `new Terminal(...)` → `term->set_font(g_font)`(别漏)→ `WindowManager::instance().add_window(term)` → `PIT::set_tick_callback(gui_tick_callback, nullptr)` → `return term`。
- **`gui_tick_callback`**:排空 `Mouse::event_queue()`(鼠标事件→`handle_mouse`,键盘事件→`handle_key`)之后,对 `wm.focused()` 做 `static_cast<Terminal*>` 再调 `poll_output()`(这章休眠,见边界)和 `render_to_canvas()`,最后 `wm.composite()`。
- 边界提醒:那个 `static_cast<Terminal*>(focused())` **没有类型检查**,赌的是"当前前台一定是 Terminal"。本 lab 只有一个终端窗口,成立;别假装它对任意窗口类型都安全。

## 接口约束

- 窗口网格 `COLS=80` / `ROWS=25`,像素尺寸 `640×400`;`TerminalCell` 默认 `ch=' '`,`fg=0x00FFFFFF`,`bg=0x00000000`。
- `on_key` 只处理 `ev.pressed && ev.ascii != 0`;松开和功能键(ascii==0)直接 return。
- `on_key` 的双分支语义(本 lab 只走第二支,但接口要写对):若 `stdin_pipe_` 非空,把 `ev.ascii`(`'\r'` 转 `'\n'`)`try_write` 进管道后**立即 return、不本地回显**;否则 `process_char(ev.ascii)`。
- ANSI 只接 `ESC[2J` / `ESC[H` / `ESC[K` / `ESC[m` 四个 final byte;`'m'` 忽略,`'H'` 无条件归零,参数只解析单个数值。
- `render_to_canvas` 用基类 `canvas()`,内容 Y 偏移 `TITLE_BAR_HEIGHT`(=20)。

## 验证步骤

**第一步:host 单元测试**(`test/unit/test_terminal.cpp` 用 `MockTerminal` 镜像重画字符逻辑,不碰硬件):

```bash
ctest --test-dir build -R "terminal|window|canvas" --output-on-failure
```

预期:`Terminal` 构造初始化空格屏、光标(0,0)、尺寸 640×400、`write` 的各种控制字符、`backspace`、`tab`、`clear`、`on_key` 只接可打印字符、`ESC[2J/H/K`、常量 `COLS=80/ROWS=25` 全过;`window`/`canvas` 验 `on_key` 虚派发与 `blit` 负坐标裁剪。

**第二步:QEMU kernel 测试**(真内核 `Terminal`,依赖 Framebuffer/PSFFont/heap):

```bash
cmake --build build --target run-big-kernel-test
```

预期:`run_terminal_tests()` 与 `run_window_manager_tests()`(含新增的"`on_key` 经基类指针派发到子类"那组)通过。退出码约定:全过写 `exit_code=0`,经 `isa-debug-exit` 后 QEMU 退 1,脚本判 `[ $QEMU_EXIT -eq 1 ]`。

**第三步:视觉效果**:

```bash
cmake --build build --target run
```

预期:开机进 GUI,屏幕中央一个 `Cinux Terminal` 窗口;鼠标点中它(置顶拿焦点),敲键盘,字符带反色光标出现在窗口里;写满一行换行、写满一屏滚动。这章只是**本地回显**——终端还不知道你在敲命令,这是正常的。

## 常见故障

- **窗口打开一片空白**:九成是 `font_` 没注入。`render_to_canvas` 和 `on_paint` 都有 `font_ == nullptr` 守卫,字体指针靠 `set_font` 从外部注入,忘了调或 `g_font` 还是空,整段光栅化直接跳过。先查字体指针,别怀疑 `draw_text`。
- **敲一个键出现两个字符**:`on_key` 的本地回显分支和(将来的)管道回显分支同时走了。规矩是:回显永远只走一条路。本 lab 没挂管道,只该走 `process_char`;挂了管道就必须 `try_write` 后 `return`、绝不本地画。
- **滚动时越界 / 屏幕花一下**:`newline()` 里先 `scroll_up` 后钳 `cursor_y_` 了,导致光标短暂指向 `screen_[ROWS][...]`。必须先钳到 `ROWS-1` 再滚。
- **ANSI 解析卡住或吞字符**:`handle_ansi` 没把 `pos` 推进到序列末尾(`write` 的循环就跳不出转义段),或没处理"ESC 后面不是 `[`"的情况(要 `pos++` 跳过这个孤立的 ESC)。
- **`on_paint` 没效果**:你在 `on_paint` 里用了它传进来的 `canvas` 形参。Terminal 画的是基类自己的 `canvas()`,形参是被忽略的。
- **`set_pipe_write_fd` / `pipe_write_fd` 看着像按键通道,但接了没反应**:这俩在本 tag 是 **dead code**(全仓无生产调用),历史遗留的"裸 fd"方案残骸。真实按键通道是 `stdin_pipe_->try_write`,那是 Lab 031b 的事——本 lab 别去碰 `set_pipe_write_fd`。

## 通过标准

- [ ] host `-R "terminal|window|canvas"` 全绿,`test_host` 整体不回归。
- [ ] `run-big-kernel-test` 里 `run_terminal_tests()`、`run_window_manager_tests()` 通过。
- [ ] `Window` 有虚析构和 `on_key`/`on_paint` 虚函数(基类空实现);`WindowManager::handle_key` 把事件透传给 `focused_->on_key`。
- [ ] `Terminal` 继承 `Window`,80×25 字符缓冲 + 光标,`write` 正确处理 `\n\r\b\t` 与可打印字符,触底正确滚动,极简 ANSI(`2J/H/K`,忽略 `m`)。
- [ ] `render_to_canvas` 用 PSF glyph 光栅化 + 反色光标块,内容画在标题栏下方;`gui_start` 返回 `Terminal*` 并 `set_font`。
- [ ] 在代码或报告里**诚实标注**两条边界:`static_cast<Terminal*>(focused)` 是硬编码假设(无 RTTI);`set_pipe_write_fd`/`pipe_write_fd` 是 dead code,真实按键通道走 `stdin_pipe_->try_write`(Lab 031b 点亮)。不把未接线的东西写成已工作。
