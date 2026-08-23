# P5-e — 终端 ANSI 彩色渲染（SGR fg / 光标 / 清屏）

> P5 第五批（收尾）。P4-c 的 TerminalWidget 把 ANSI 转义**吞掉**（不渲染彩色）。P5-e 升级为**解析执行**：SGR 设前景色、光标定位、清屏 —— 真彩色终端。P5 全完成。

## 背景

P4-d terminal-host 跑真 shell，但 ANSI（`\e[31m` 红色 / `\e[H` 光标 / `\e[2J` 清屏）被 P4-c 状态机整段吞（不显示残渣，但也没彩色/光标）。shell 输出（ls --color / prompt / clear）缺彩色 + 光标命令失效。P5-e 解析执行这些，真终端体验。

## 目标

TerminalWidget 解析 CSI SGR(`m`)/光标(`H`)/清屏(`J`)；per-cell 前景色；16 色 palette 渲染。Host ABI 零改动。

## 设计

- **cells 加并行 `fg_colors_[]`**（uint8_t SGR 索引 0-15，与 cells_ 同布局）。`cur_fg_`（SGR 当前，默认 7=白）。put_char_ 可打印时 `fg_colors_[idx] = cur_fg_`（stamp 当前色）。
- **ANSI 状态机升级**：kCsi 收集 param bytes（0x20-0x3F：数字 `;` 等）到 `csi_param_[16]`，final byte（0x40-0x7E）调 `dispatch_csi_(final)`。
- **dispatch_csi_**：`m`→`apply_sgr_()`；`H`/`f`→光标定位（parse `row;col`，1-based，clamp）；`J`→`2J` 清 cells；其他吞。每 CSI 后 `csi_len_=0`。
- **apply_sgr_**：parse `;`-分隔数字。0/39→reset(7)；30-37→fg=code-30；90-97→fg=code-90+8（bright）。bold(1)/bg(40-47) 等忽略。
- **16 色 palette**（kAnsiPalette[16]，XRGB8888）：标准 ANSI（黑红绿黄蓝品青白 + bright）。paint 每 cell `text_glyph(kAnsiPalette[fg & 0xF])`。
- **scroll 带 color**：scroll_up_ 同时滚 fg_colors_（色随字）。
- **`fg_at(col,row)` public**（测试查 fg 索引）。

## 决策

1. **fg only（bg 暂默认黑）**：SGR bg（40-47）+ cell bg 要 per-cell bg_colors_ + paint bg fill per cell（每 cell 两原语：bg fill + fg glyph）。工作量倍增。P5-e 先 fg（最常见，ls/prompt 彩色够用），bg 留后续。
2. **SGR 子集（30-37/90-97 fg + 0/39 reset）**：覆盖绝大多数 shell 彩色输出。bold(1)/italic/bg/256 色（38;5;N）等忽略（cur_fg_ 不变）。够 ls --color / 彩色 prompt。
3. **光标 `[row;colH`（1-based）+ `[2J` 清屏**：shell `clear` / 定位输出工作。其他 CSI（K 擦行 / S 滚 / A/B/C/D 移动）忽略。
4. **cells 保持 char + 并行 fg_colors_**（不 struct{char,fg}）：不改 cells_ 布局（cell_at 不变），fg_colors_ 独立。scroll 同步两数组。

## 陷阱

- **scroll 要滚 fg_colors_**：只滚 cells_ → 滚后颜色错位（字上了，色留原行）。scroll_up_ 两数组都滚。
- **CSI param buffer 重置**：每 CSI 收集前 `csi_len_=0`（kEsc→kCsi 时）+ dispatch 后清。否则跨 CSI 残留。
- **`fg_at` clangd 误报**：hpp 声明 + cpp 定义签名一致，clangd 缓存报"不匹配"。gcc 编译过。
- **bright 映射**：90-97 → index 8-15（code-90+8）。palette[8-15] bright 色。

## 验证

- 新 `test_terminal_ansi.cpp` 5 段：SGR fg（31 红 / 32 绿）+ reset（0/39 白）+ bright（91→9）+ 光标 `[1;1H`覆盖 + `[2J`清屏。
- standalone ctest **19/19** + ASAN 干净，零回归。
- Host ABI 零改动；core 仍 host-neutral；纯整数（palette 查表，无浮点）。

## CinuxOS 侧

零改动。

## P5 收尾

**P5 增强 ✅ 全完成**（5 批 a-e）：
- P5-a 字体整数缩放 + 文本测量
- P5-b 主题配色变体 + 运行时切换
- P5-c per-widget dirty + Desktop idle（0 flush）
- P5-d flex 权重布局 + per-corner 圆角
- P5-e 终端 ANSI 彩色（SGR/光标/清屏）

ctest 19/19 + ASAN；Host ABI 全程零改动。下一步候选：per-widget 局部 dirty（只刷脏区，→ Scene 退役）/ ANSI bg + 256 色 / 更大字体（搬 PSF）/ 字体抗锯齿 / CinuxOS 删 kernel/gui/（P4 决策③留的）。
