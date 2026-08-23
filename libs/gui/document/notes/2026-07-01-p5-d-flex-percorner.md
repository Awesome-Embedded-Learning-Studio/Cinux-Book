# P5-d — flex 权重布局 + per-corner 圆角

> P5 第四批。HBox/VBox 等分 → flex 权重；swraster fill_rounded_rect 加 per-corner（Window 标题栏顶圆，不再被方角覆盖）。

## 背景

P3-c HBox/VBox 等分（无权重，标题栏+关闭键布局表达不了）；fill_rounded_rect 四角全圆（Window 标题栏矩形覆盖 body 顶圆角，"上方角下圆角"妥协）。P5-d 加 flex + per-corner 解两债。

## 目标

Widget flex_（HBox/VBox 权重）+ swraster per-corner 圆角（kCorner 位掩码）+ Window 标题栏顶圆。Host ABI 零改动。

## 设计

- **Widget `flex_`（默认 1）+ `set_flex`/`flex()`**。HBox/VBox layout 按 flex 权重分：`child 宽 = avail * flex / total_flex`，**last child 拿余数**（不丢像素）。
- **swraster `fill_rounded_rect` 加 `corners` 参数**（kCornerTL/TR/BL/BR 位掩码，默认 kCornerAll）。每行角弧只对启用角 bite in；方角 bite 0。签名 `(... radius, clip, corners=kCornerAll)`（默认参数在末尾）。
- **PaintList `fill_round_rect_corners`**（FillRoundRectCmd 加 corners 字段）；`fill_round_rect` 默认 0xFu（全圆，兼容）；execute 传 `cmd.corners`。
- **Window 标题栏 `fill_round_rect_corners(TL|TR)`**：顶角圆（替代 fill_rect 方角覆盖），底方接 content。body 仍全圆（content 透明露出底圆）。

## 决策

1. **flex 默认 1 = 等分兼容**：旧等分（cwid=avail/n 整除，**丢余数**）；新 flex last 拿余数（avail - used）。等分场景默认 flex 1，行为近等分但 last 多余数像素。test_widgets 适配（last c.w 93→94）。
2. **per-corner 默认 kCornerAll（向后兼容）**：fill_rounded_rect(... radius, clip) 默认全圆；显式 corners 只圆选中角。签名 corners 放 clip 后（C++ 默认参数末尾）。
3. **Window 标题栏顶圆（TL|TR）**：替代 P4-a 的"标题栏方角覆盖 body 顶圆角"妥协。现标题栏顶圆 + body 全圆 + content 透明 → 四角协调（顶圆来自标题栏，底圆来自 body）。
4. **swraster per-corner 复用 isqrt**：每行算 top_d/bot_d，启用角 bite in（off = r - isqrt(r²-d²)）。方角 bite 0。无新数学。

## 陷阱

- **flex 余数分配改了 last child 宽**：旧丢余数（avail/n 整除），新 last = avail - sum。test_widgets HBox 3-child（avail=280）：旧全 93，新 93/93/**94**。c.w 断言 93→94。
- **C++ 默认参数顺序**：首版 `corners = kCornerAll, clip`（默认后跟非默认）→ 编译错。改 `clip, corners = kCornerAll`（默认在末尾）。
- **fill_round_rect 聚合初始化**：FillRoundRectCmd 加 corners 后，`{x,y,w,h,color,radius}` 缺 corners → 改 `{..., radius, corners}` 或委托（fill_round_rect → fill_round_rect_corners(0xFu)）。

## 验证

- 新 `test_flex.cpp`：HBox 3:1（75/25）+ default 等分（33/33/34 last 余数）+ per-corner（TL|TR 顶圆底方像素）。
- widgets-test 适配（last c.w 94）。**ctest 18/18 + ASAN 干净**。
- Host ABI 零改动；core 仍 host-neutral；纯整数。

## CinuxOS 侧

零改动。

## 下一步

P5-e 终端 ANSI 彩色渲染（SGR 16 色 fg/bg + 光标定位/清屏执行，TerminalWidget cells 加 color 属性）。
