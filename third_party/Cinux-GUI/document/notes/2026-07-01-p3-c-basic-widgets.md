# 2026-07-01 · P3-c · 基础控件 + 线性布局(Label/Button/Container + HBox/VBox)

> P3 第三批,用 P3-a 骨架 + P3-b 圆角/配色做出真控件:Label(文字)、Button(Material contained + press 状态)、Container + HBox/VBox(横/竖均分布局)。Widget 加 `layout()` hook,Desktop render 前 layout 把孩子位置定好。

## 背景

P3-a/b 有了控件骨架(Widget/PaintList/Desktop)+ Material 外观能力(圆角 + theme),但还没「能点能排版」的真控件。本批填上:三个基础控件 + 两个布局容器,够拼一个 Material 风格的面板。

## 目标

1. **Label**:可选 bg + 左上 padding 文字。无交互。
2. **Button**:Material contained(rest = primary 圆角 + on_primary 文字;press = surface + primary 文字)。down→pressed、up→released。
3. **Container + HBox/VBox**:Container 可选 bg;HBox/VBox 等分子件 + gap + padding。
4. **Widget::layout() hook**:Desktop::render 前先 root->layout() 递归排版。

## 设计

### Widget 加 `virtual layout()`(默认 noop)

容器 override:HBox/VBox::layout() 算每个 child 的 rect(set_rect)+ 递归 child->layout()(嵌套容器沉底)。Desktop::render 在 flatten 前调 root->layout()。

### `core/widget/{label,button,container}.{hpp,cpp}`(子目录)

- **Label**:`paint_to_list` = (bg!=0 → fill_rect) + (text 非空 → text 左上+4px)。
- **Button**:`paint_to_list` = fill_round_rect(pressed? surface:primary, theme.button_radius) + text(pressed? primary : on_primary)左上+8px。`on_pointer`:down→pressed=true,up→pressed=false。配色全从 Theme 读(theme_ 指针,nullptr 兜底默认 Material 色)。
- **Container**:paint_to_list = (bg!=0 → fill_rect)。HBox::layout = 子件等分宽 `(cw-gaps)/n` + spacing 横排;VBox 同理竖排。padding 内缩内容区。

### 布局整数算术(无 flex,决策 C)

`child_size = (content - gaps) / n`(整除,余数丢)。`child_i 位置 = pad + i*(child_size + spacing)`。简单线性,够 P3;flex 权重 / preferred-size 协商后批。

## 决策

- **Button press = 反色(surface+primary),非 elevation 变深**:纯整数算不起 blur 阴影;rest=primary、press=surface 视觉跳变明显,且不依赖阴影。Material 的「press 深 12%」要色彩插值 + 阴影,留后。
- **控件放 `core/widget/` 子目录**:P3-a 平铺(widget.hpp),P3-c 控件多了开子目录隔离。CMake PUBLIC include core/,`#include "widget/label.hpp"` 经 include path 解析(`../widget.hpp` 相对回上级)。
- **layout() 虚函数 + Desktop 自动调**:容器 override layout,Desktop render 前 root->layout()。host 不用手动 layout(自动)。layout 是「整棵树一次」(每帧),简单;增量 layout(仅脏子树)后批。
- **Label/Button 文字左上对齐,不居中**:首批简单。居中要文字宽度测量(font metrics),P5 字体批再做。

## 陷阱

- **HBox/VBox 整除余数丢**:`(cw-gaps)/n` 整除,3 件 300px/10gap → 每件 93(总 93*3+10*2=299,丢 1px)。可接受(1px 不影响视觉);若要精确填满,余数分给前几件 —— 后批。
- **Button 圆角 vs 文字位置**:文字 (x+8, y+8),圆角 radius 8 → 文字在圆角弧内?(x+8,y+8) 距角顶 8px,radius 8 角弧咬进 ~3px(off at row 0 = r-isqrt(r²-(r-1)²)),8 > 3,文字在弧内安全区。但文字字符宽 8px(font),「OK」占 16px,在 100px 按钮里居左,OK。

## 验证

- **standalone ctest 全绿**(Release):9/9 —— 新 `widgets-test` 5 段:Label(bg + glyph 命中)/ Button(rest primary + down→pressed→surface + up→released)/ HBox(3 件 93px + gap 几何)/ VBox(2 件 145px + gap)/ Desktop(HBox+2 Button,两 primary body 上屏)。
- **ASAN 干净**:`widgets-test`/`theme-test`/`widget-test`/`compositor-test` exit 0。
- **clang-format 自洽**:widget/*.{hpp,cpp} self-diff = 0。

## 后续

- **P3-d(🔜,最后一批)**:Slider(拖动取值,press capture + move 改 value)+ replay/fbdev 控件 demo(Material 外观真跑)+ 三 host 切控件树、P2 Scene 退役(决策 A① 兑现)。
- CinuxOS 侧零改动。
