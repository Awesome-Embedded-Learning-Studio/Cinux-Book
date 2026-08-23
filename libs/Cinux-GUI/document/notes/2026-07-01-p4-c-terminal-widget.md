# P4-c — TerminalWidget（字符终端显示）

> P4 第三批。本仓立**字符网格终端控件** —— CinuxOS `kernel/gui/terminal.cpp`(80×25 Canvas 逐像素 render_to_canvas + Pipe 取字节)本仓重建为 Widget + PaintList。IO 先注桩(P4-d 接 PTY)。

## 背景

P4-d 要 terminal-host 跑真 shell,先得有"显示终端"的控件。CinuxOS terminal 缠 Pipe + 渲染到 Canvas;本仓显示层 host-neutral 重建(Pipe/PTY 留 P4-d host 层)。本批只做**显示**:喂字节 → 字符缓冲 + 滚动 → PaintList 渲染。

## 目标

host-neutral TerminalWidget:cols×rows 字符网格 + cursor + 换行/回退/滚动 + 用 text_glyph 渲染。IO 是个 write(bytes) 方法(P4-d 接 PTY)。不碰 Host ABI。

## 设计

**TerminalWidget : Widget**,持 `cells_[kMaxCols * kMaxRows]` 字符网格(stride = kMaxCols,cols_ 可变小不变布局)。

- **write(bytes)**:逐字节 `put_char_`。
- **put_char_**:`\n`→换行;\r→col=0;\b→col--;\t→col 对齐 8;可打印(≥0x20)→ 写 cell + col++,满列自动换行;其他控制字节丢弃(暂不解析 ANSI)。
- **换行/滚动**:row++ 到 rows_ → `scroll_up_`(行 1..rows-1 上移、末行清空),row=rows-1。
- **paint_to_list**:`fill_rect`(bg 黑)→ 每个非空 cell `text_glyph`(x0+col*8, y0+row*16, fg 白, ch)。空 cell 跳过。

## 决策

1. **新增 `kTextGlyph` cmd(内联单字符)**:`kText` 借用 `const char*`(须活到 execute),终端逐字符渲染借不了栈临时。`kTextGlyph` 把 char 内联进 cmd(无借用),execute 走 `glyph_blit` 单字符。per-cell 无需持久缓冲。
2. **扩 `PaintList::kMaxCmds` 256→4096**:终端 80×25 = 2000 字符 cmd + 周围控件,256 远不够。4096(~128KB 栈帧 `sizeof(PaintCmd)≈32`)容纳满载终端 + 余量。
3. **cells stride = kMaxCols(非 cols_)**:cols_ 可 set_cols_rows 变小,但 cells 布局用 kMaxCols 固定 stride,避免重布局。
4. **暂不解析 ANSI**:纯文本 + \n/\r/\b/\t;raw PTY 的 ANSI 字节当控制字节(<0x20)或字面量显示。P5/后续加 ANSI 子集(清屏/光标移动)。

## 陷阱

- **kText 借用 vs 逐字符**:首版想用 kText + per-cell 局部 `char[2]` 缓冲 → 局部变量 paint_to_list 返回即失效 → dangling。kTextGlyph 内联规避。
- **kMaxCmds 与栈**:4096 cmd 在 `Desktop::render`/`flatten` 栈上 PaintList,~128KB。调用栈不深,无 stack overflow。ASAN 验证干净。
- **滚动时清末行**:scroll_up_ 末行不清则残留;清 0(空 cell paint 跳过)。

## 验证

- 新 `test_terminal.cpp` 6 段:写字符 / `\n` 换行 / `\r` 覆盖 / 滚动(3 行写 4 行) / `\b` 回退 / flatten 含 kFillRect + kTextGlyph。
- standalone ctest **14/14** + ASAN 干净。
- Host ABI 零改动;core 仍 host-neutral;纯整数 + 定长。

## CinuxOS 侧

零改动。CinuxOS `kernel/gui/terminal.cpp`(Canvas + Pipe)暂留对照。

## 下一步

P4-d Terminal IO + 首实装 spawn:Linux host `forkpty` + exec `/bin/sh`,PTY master fd 双向;terminal-host SDL2 主循环(PTY read → terminal.write;键盘 → PTY write;render)。spawn 用 PTY 内部、pipe 签名不变(Host ABI 零改动)。
