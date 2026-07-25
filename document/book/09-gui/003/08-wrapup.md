---
title: 08 · 收尾:验证 + 没做的 + 小结
---

# 收尾:验证 + 没做的 + 小结

## 验证

这一章的验证分三层:host 单元测试(纯逻辑)、ASAN 干净(内存)、真 shell 手动冒烟(视觉效果)。

**第一层:host 单元测试。** 字符缓冲、ANSI、脏区这些纯逻辑不碰真硬件,在 host 上用 ctest 测。终端相关的测试套覆盖了:`test_terminal.cpp`(写字符 / `\n` 换行 / `\r` 覆盖 / 滚动 / `\b` 回退 / flatten 含 kFillRect + kTextGlyph)、`test_terminal_ansi.cpp`(SGR fg 31 红 / 32 绿、reset 0/39、bright 91→9、光标 `[1;1H` 覆盖、`[2J` 清屏)、`test_terminal_bg256.cpp`(bg SGR 41/42、256 色 38;5;200、reset 48;5;100→0、cursor block flatten 含 ≥2 个 fill)。跑法:

```bash
cmake -S third_party/Cinux-GUI -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -R "terminal|window|widget|dirty|cursor" --output-on-failure
```

这一组还顺带验了 Widget 框架本身——`test_widget.cpp` 覆盖 hit-test 嵌套 child / dispatch 交付 / flatten→execute 像素 / clip 收敛(child 超 parent rect 画不出去),`test_window.cpp` 验 Window 的几何/hit/拖拽/close,`test_window_manager.cpp` 验 Z 序/raise/click-to-raise/cursor 跟踪。

**第二层:ASAN 干净。** host 单测在 push 前开 ASAN 自验,确保虚析构链、new[]/delete[]、借用指针(text cmd 的 `const char*`)都没漏:

```bash
cmake -S third_party/Cinux-GUI -B build-asan -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-fsanitize=address" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

这一层特别能抓"PaintList 借用指针 dangling"这类问题——`kText` 的 `const char*` 如果调用方传了栈临时,ASAN 会立刻报 use-after-return。

**第三层:真 shell 手动冒烟。** 想亲眼看一个跑着 shell 的终端:

```bash
cmake -S third_party/Cinux-GUI -B build -DCINUX_HOST_TERMINAL=ON
cmake --build build -- terminal-host
./build/terminal-host          # WSL2: 经 WSLg 显示;敲 shell 命令
```

预期:开一个 SDL 窗口,里头是一个跑着 `/bin/sh` 的终端窗口;鼠标点中它(被顶到最前)、键盘敲命令,字符带着白色光标块出现在窗口里;`ls --color` 出彩色目录清单;`clear` 清屏 + 光标归位;写满一行自动换行,写满一屏向上滚动;标题栏可拖、关闭键可点。这一步看到 shell 真的"听话"地执行命令、结果带颜色地回显,就说明从键盘到 PTY 到 `write` 到 `paint_to_list` 到 Compositor 的整条链路通了。

这一步是非 CI 的眼检——ASAN 抓不到的视觉效果(光标拖影、脏区漏刷、颜色错位)只能靠眼睛。但前面两层已经把逻辑对齐过了,这一层主要验"host 层接线对不对"(PTY 双向通、键盘双路对、per-rect upload 对)。

## 这章没做的

把边界划清楚,免得你以为这套终端已经无所不能。

- **没有 scrollback。** 滚出屏幕顶端的内容永久丢弃,没法往上翻。`scroll_up_` 把顶行覆盖、不留历史缓冲——这是有意简化(真 scrollback 要一张 ring buffer + 上滚时按 PgUp 重画,牵动 dirty 策略,先不做)。
- **光标不闪烁、不反色。** TerminalWidget 的光标是 always-on 的白色实心方块,盖在当前 cell 上(那个字符被遮住)。真终端的光标是反色透出字符,实现要画两步(前景铺满 + 用 bg 重画 glyph),还要定时器驱动闪烁——这章不折腾。
- **ANSI 只做子集。** CSI 支持 SGR `m`(fg/bg/256 色)、光标定位 `H`/`f`、擦屏 `J`、光标移动 `A`/`B`/`C`/`D`;擦行 `K`、滚屏 `S`/`T`、多参数定位明确不做。SGR 里 bold(1)、italic、truecolor(`38;2;r;g;b`)忽略——24-bit per cell 会让 `fg_colors_`/`bg_colors_` 从 1 字节涨到 4 字节,内存翻倍,优先级低。
- **OSC 整段吞。** `ESC ]` ... `BEL` 之间的字节全部消费掉不画——不实现设窗口标题、超链接,但也不让这些字节当普通字符画成乱码。
- **键盘不经 Widget 树。** TerminalWidget 没有 `on_key` override,host 直接 `pty_write` 进 PTY master。这不是缺陷,是终端和普通输入控件(后面的 TextBox)的本质区别:普通控件是"键盘 → 控件状态",终端是"键盘 → shell → 回显 → 控件"。
- **read 每帧封顶 8192 字节。** shell 大爆发(`ls /usr/lib`、加载 bashrc)分摊到多帧,牺牲一点输出速度换主循环响应——GUI 单线程模型下不能让一帧的 `put_char_` 跑几万次卡住。
- **本章的 host 是 SDL2 + POSIX forkpty。** 两者都在 ring3、同一个 Linux 主机。真搬到 Cinux 内核里跑,host 层要换:字节通道换成内核 PTY([007](../07-userland/007/))或 AF_UNIX socket([005](../17-net/005/)),事件/画像素换成 `/dev/event0` + `/dev/fb0`([012](../012/))。Widget 树和 PaintList 一行不改——这就是 host-neutral 的意义。
- **保留模式不自动恢复遮挡背景。** Window 移走、Window 关闭露出的那块,都要有人显式 `invalidate` 旧 footprint;即时模式全屏重画自动解决,保留模式必须显式。本章调试现场三个坑(光标拖影、窗口移动、窗口关闭)都是这一条的不同表现。

## 下一站

031 的终端已经能跑真 shell、能出彩色、能滚动。可这个 shell 是从开机那一刻就在跑的——host 一启动就 `forkpty` 出一个 `/bin/sh`,挂在唯一的那个终端窗口上。桌面要是只有一个终端窗口,这套没问题。可 033 要把桌面变成"有一排图标、点哪个开哪个"——终端不再是开机默认出现的那个窗口,而是"点 Shell 图标才该出现"的窗口之一。

这就撞上一个时序矛盾:如果开机就造终端,桌面一进来就有一个终端杵那儿,违背"点图标才开"的交互;可如果开机不造终端,shell 又是开机就起的(它得是第一个 ring-3 进程),它一跑就往 stdout 写,谁来接?

答案在 033b——"懒创建":shell 照旧开机起,它的 stdio 照旧挂在 PTY 上;但 PTY 的另一端不立刻绑终端,而是把 fd 先存进 host 状态;用户点 Shell 图标那一刻,host 才 `new` 一个 TerminalWidget + Window、把它们推上桌面、把存好的 fd 绑上去。在终端出生之前,shell 写出的字节先在 PTY 缓冲里排队。这套"懒创建"的代价是要认真对待"生产者(shell)先于消费者(终端)"的那段时间——这正是 033b 要细讲的地方。

至于 GUI host 搬到 Cinux 内核跑、ring3 进程经 `/dev/event0` 读事件、`/dev/fb0` mmap 画像素——那是 087 的事。这一章的 Widget 树 + PaintList 一行没改,只是 host 层从 SDL 换成了那两条设备接口。字节通道从 POSIX `forkpty` 换成内核 PTY(066)或 AF_UNIX socket(083),也是 host 层的事。core 对这些一无所知——它只认识 Widget、PaintList、Surface,这就是 host-neutral 的意义。

## 小结

031 把 030 那个"会动的空窗口骨架"变成"窗口里真能跑 shell"——但比"加一个终端控件"更重要的,是顺手把整套渲染模型从即时模式换成了保留模式。记住下面几条就够:

- **Widget 基类的接口名是 `paint_to_list(PaintList&)`,不是 `on_paint`**;`flatten` 才是非虚框架入口(clip push → paint_to_list → 递归 child → clip pop),`paint_to_list` 是子类填的 protected virtual hook。`hit_test` / `on_pointer` / `on_key` 是另外几个虚 hook。
- **PaintList 是定长 4096 的 cmd 数组**,7 种 CmdKind;溢出 drop 不 abort(守"core never aborts"铁律)。`kTextGlyph` 单字符内联进 cmd,是为了避免字符密集型控件借用栈临时 `char[]` 当指针导致 dangling——这是这种控件最容易踩的坑。
- **保留模式三红利**:脏区重绘(只重画标了脏的矩形)、批量合成 + clip 栈裁剪(控件画不出祖先矩形)、跨进程共享(PaintList 是纯数据,087 把 GUI host 搬用户态的地基)。
- **ANSI 不能靠猜**:BS(`\b`)只移光标、DEL(0x7f)才擦字符——shell 行编辑的左箭头/退格全靠这条分开才不"吃字"。
- **任何"位置会移动 / 会消失"的可视元素,旧位置 + 新位置都得显式标脏**:光标行(`prev_cursor_row_`)、窗口移动(`move_to_` 标 old footprint)、窗口关闭(`remove_window` capture stale rect)。这是保留模式相对即时模式最容易漏的一类点,本章调试现场踩了三次。
- **WindowManager 自管 `windows_[]` 不用 `children_`**(因为 flatten 是 self→children 序,画不了"光标在最上");连带 `collect_dirty`/`clear_dirty` 必须 override 显式递归 `windows_`,否则 Window 的脏区永远到不了 root。
- **flatten 一次、execute per-rect(幂等)**:N 个脏区跑 N 遍整张 list,clip 外的 cmd O(1) 跳过,N 通常 2~3。
- **终端"输入不经控件、输出才进控件"**:host 把键盘字节直接 `pty_write` 进 PTY,不经 `Desktop::dispatch_key`;shell 输出 read PTY → `term.write` 才进控件。TerminalWidget 因此没有 `on_key` override。
- **forkpty 而非裸 pipe**:PTY 给 shell 一个控制终端,行编辑、历史、curses 才能用;`*stdin_fd = *stdout_fd = master` 签名跟 pipe 一样、内部是 PTY。

这一章之后,Widget 树 + PaintList 这套地基就立住了。后面不管是再加控件(按钮/文本框/滑块,Widget 库里其实都已经在了)、把 GUI host 搬进 Cinux 内核(087)、还是换字节通道(066 PTY / 083 AF_UNIX),都是在这套地基上加 host 适配、不再动 core。031 立的不是"一个终端",是"userspace GUI 的控件框架 + 渲染模型"。

## 参考

- ECMA-48 — Control Functions for Coded Character Sets,5th edition(1991 年 6 月)。CSI 序列:`ESC[m` SGR 设色、`ESC[H` CUP 光标定位、`ESC[J` ED 擦屏、`ESC[A/B/C/D` 光标移动。38;5;N / 48;5;N 的 256 色扩展见 xterm 的 `ctlseqs`:https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- xterm 256 色 palette(0-15 标准 16 色、16-231 的 6×6×6 立方、232-255 灰阶),支撑 [`palette_color`](../../../third_party/Cinux-GUI/core/widget/terminal.cpp#L19-L33) 的颜色翻译算式:https://github.com/termstandard/colors
- Retained-mode vs immediate-mode GUI(保留模式 vs 即时模式的概念框架,支撑本章 PaintList 保留模式 vs 029 Canvas 即时模式的对比):https://en.wikipedia.org/wiki/Graphical_user_interface#Modes
- Linux `forkpty(3)` / PTY(控制终端、行编辑、curses,支撑 [`linux_spawn`](../../../third_party/Cinux-GUI/host/posix_spawn.cpp#L16-L40) 用 PTY 而非裸 pipe 的选择):https://man7.org/linux/man-pages/man3/forkpty.3.html
