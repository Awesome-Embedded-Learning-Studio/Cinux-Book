# P5-a — 字体整数缩放 + 文本测量

> P5 第一批。PSF 8×16 在桌面标题/终端偏小 —— 不搬更大字体，用**整数缩放渲染**（8×16 → 16×32 / 24×48）+ 文本测量。零新字体数据，nearest neighbour。

## 背景

P4 桌面跑起来后，8×16 字体在窗口标题/控件偏小。搬更大 PSF 要找/生成字体数据；缩放（swraster 整数 scale）零新数据，立即可用，粗糙但可读。顺带补文本测量（width/height），让控件能算文字尺寸（居中/裁剪/换行的基础）。

## 目标

swraster `glyph_blit_scaled` + PaintList `kTextScaled` + compositor `draw_text_scaled` + font `text_width/text_height` + Label `set_scale`。Host ABI 零改动。

## 设计

- **swraster `glyph_blit_scaled(s, x, y, bits, gw, gh, scale, color, clip)`**：同 `glyph_blit` 的 MSB-first bit 布局，但每个 set bit 画 **scale×scale 块**（复用 `fill_rect`，clip 自动）。scale==1 = `glyph_blit`。
- **PaintList `kTextScaled` cmd**：`TextScaledCmd{x, y, color, scale, str}`（str 借用，同 kText）。`text_scaled(x,y,color,str,scale)`。
- **compositor `draw_text_scaled`**：同 `draw_text` 但 `glyph_blit_scaled` + `\n` 换行用 `height*scale`。execute 加 `kTextScaled` case 分发。
- **font `text_width(f, str, scale)`**：最长行宽度（`\n` 分行），`lines * width * scale`。`text_height(f, str, scale)`：行数 × `height * scale`。多行感知。
- **Label `set_scale`**：`scale_` 成员（默认 1）；paint 时 `scale_>1` 用 `text_scaled` 否则 `text`。

## 决策

1. **缩放而非搬更大 PSF**：零新字体数据（8×16 PSF 已在仓），整数 scale 简单（swraster 复用 fill_rect）。nearest neighbour 字形粗糙但可读；要抗锯齿/高质量得搬矢量/更大位图字体（远期）。P5-a 解决"太小看不清"，不解决"丑"。
2. **每 bit 画 scale×scale 块（fill_rect）**：不写独立 pixel 循环，复用 fill_rect 的 clip/bounds。相邻 bit 块不重叠（col×scale 间隔），故 scale 2 = 4× pixel 精确。
3. **kTextScaled 借用 str（同 kText）**：Label/Window 的 text_ 成员稳定，借用安全；不学 kTextGlyph 内联（终端逐字符才需内联）。
4. **Label 先用，Window/Button 留**：P5-a 聚焦缩放基建 + Label；Window 标题/Button 用缩放可通过 content Label 或后续小改。基建（swraster/paint_list/compositor/测量）通用。

## 陷阱

- **font.hpp ≠ theme.hpp**：首版把 text_width/height 声明加到 theme.hpp 的 `material_dark` 后（锚错文件）。font.hpp 末尾是 PsfFont class，用其 private 成员 + namespace 闭合作锚。
- **-Wswitch**：加了 kTextScaled enum 值后，execute 的 switch 没立刻加 case → clangd 提醒。enum 扩展必须同步 execute 分发。
- **n2 == n1×4 前提**：相邻 bit 的 scale×scale 块不重叠（col 间隔 scale）。若写"逐 pixel 放大"循环要注意别重复设。

## 验证

- 新 `test_font_scale.cpp` 3 段：text_width/height（单行/多行/双 scale）+ glyph_blit_scaled（scale 2 = 4× scale 1 pixel）+ kTextScaled 经 execute 像素 == 直接 glyph_blit_scaled。
- standalone ctest **16/16** + ASAN 干净。
- Host ABI 零改动；core 仍 host-neutral；纯整数（缩放无浮点）。

## CinuxOS 侧

零改动。

## 下一步

P5-b 主题运行时切换（light/dark set_theme 重渲染）+ 配色变体 + sdl-host 按键切换 demo。
