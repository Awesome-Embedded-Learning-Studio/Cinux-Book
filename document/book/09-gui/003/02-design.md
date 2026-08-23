---
title: 02 · 设计图
---

# 设计图

## 设计图

一帧的完整旅程,在 031 里长这样:

```text
┌──────────────┐  SDL poll  ┌─────────────────────────────┐
│  键盘 / 鼠标  │───────────▶│ host 主循环(terminal-host) │
└──────────────┘            │  · 鼠标 → wm.process_pointer │
                            │  · 键盘 → pty_write(in_fd)   │
                            │  · read(out_fd) → term.write │
                            └──────────────┬──────────────┘
                                           │ 每帧
                                           ▼
                          ┌──────────────────────────────┐
                          │ Desktop::render(staging, ..) │
                          │  1. root->collect_dirty(Region)│ ← 只收集标了脏的矩形
                          │  2. root->clear_dirty()         │
                          │  3. root->layout()              │
                          │  4. paint_list_.clear()         │
                          │  5. root->flatten(paint_list_)  │ ← 把树拍成 cmd 清单
                          │  6. for 每个 dirty rect:        │
                          │       comp_.render(staging,    │
                          │                     paint_list_,│
                          │                     font, &clip)│ ← 批量落屏(只画脏区内)
                          └──────────────┬──────────────┘
                                         ▼
                          staging Surface → host 推上屏
                          (SDL: per-rect UpdateTexture)
```

这里有两个 029/030 没有的关键动作。一是 `collect_dirty`:控件改了状态只标脏,**不立即画**;帧边界由根统一收集所有标了脏的矩形。二是 `flatten`:整棵 Widget 树递归地把自己想画的指令塞进**同一张** PaintList——每个控件 `clip_push` 自己的矩形(保证画不出去)、调 `paint_to_list` 塞自己的 cmd、递归子控件、`clip_pop`。

`TerminalWidget` 内部的模型则是一张廉价的字符网格,而不是直接操作像素:

```text
   cells_[kMaxCols * kMaxRows]   kMaxCols=120, kMaxRows=50   每个 cell = { char, fg[0..255], bg[0..255] }
   ┌────────────────────────────────────────────┐
   │ '$' 'l' 's' ' ' ...                       │ row 0   ← cur_row_
   │                                          │
   │ ...                                      │
   │                                          │ row rows_-1
   └────────────────────────────────────────────┘
   ↑ cur_col_ 指向下一个落字位置

   paint_to_list(每帧): cell → fill_rect(非 default bg) + text_glyph(ch) → PaintList
   满行: newline_() → 触底 scroll_up_() 把整屏上移一行,顶行丢弃
```

先维护语义层(字符 + 颜色索引),只在 `paint_to_list` 那一步翻译成绘制指令,这样换行、退格、清屏、滚动全都只是在廉价的字符数组上挪数据,代价极低。颜色用的是 ANSI 调色板索引(0..15 标准 16 色、16..231 的 6×6×6 立方、232..255 灰阶),`paint_to_list` 时再查 [`palette_color`](../../../libs/gui/core/widget/terminal.cpp#L19-L33) 翻成 XRGB8888 像素值。`cols_`/`rows_` 默认 80×25,但 cells_ 的 stride 固定是 `kMaxCols=120`——这样 `set_cols_rows` 把网格变小时不用重布局,只是少用几列。

