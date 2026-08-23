# P6-b — 窗口 resize + 最大化/最小化

> P6 第二批。Window 只能拖 + 关，补 resize（右下角 grip 拖调大小）+ set_maximized（填满 + 存 prev 还原）+ set_minimized（hide）。

## 背景

P4-a Window 有标题栏拖动 + close。缺 resize（调大小）+ maximize/minimize（窗口管理基本）。P6-b 补齐。

## 目标

Window 右下角 resize grip（拖调 w/h，min 40×24）+ `set_maximized(bool, Rect full)`（存 prev_rect_ + 填 full；false 还原）+ `set_minimized(bool)`（hide）。Host ABI 零改动。

## 设计

- **resize grip**：右下角 `kResizeHandle=8` 方块（`resize_handle_rect_` = x1-8, y1-8）。`hit_test` 命中 → Window 处理。`on_pointer` down @ grip → `resizing_`（记 press origin + 窗口起始 w/h）；move → `set_rect(x0, y0, rw_ow+dx, rw_oh+dy)`（clamp min 40 / title+4）+ layout + invalidate(old+new)；up → resizing off。
- **paint grip**：右下角 8×8 outline 方块（`theme.outline`），作可视手柄。
- **set_maximized(m, full)**：m=true 且未 max → 存 `prev_rect_=rect_`，`set_rect(full)`，maximized_=true；m=false 且已 max → `set_rect(prev_rect_)`（还原）。每次 layout + invalidate。
- **set_minimized(m)**：`minimized_=m` + `set_visible(!m)`（hide：Widget::flatten/hit_test 跳 invisible）。

## 决策

1. **resize grip 在右下角**（非边框）：8×8 角 grip，hit 简单（一角），够用。四边/四角 handle 复杂（8 区），P6-b 一角足够。
2. **resize 最小 40×24**：防止拖成 0（title 20 + 内容 4）。nw<40 → 40；nh<title+4 → title+4。
3. **set_maximized 存 prev_rect_**：还原用。API 形式（host/WM 传 full rect —— Window 不知桌面大小）。
4. **set_minimized = set_visible(false)**：复用 Widget visible（flatten/hit_test 跳）。不删 window（WM 仍管，restore 可见）。
5. **无标题栏 max/min button**：API only（简化）。button（close 旁 □ / _）留后续。

## 陷阱

- **resize set_rect uint32**：nw/nh int32（可能负），clamp 后 static_cast<uint32>。min clamp 在 cast 前。
- **resize grip hit 在 content 区**：grip 在右下角（content 区角）。hit_test 必须在 content 前查 grip（否则 content 吃）。顺序：close → resize → title → content。

## 验证

- 新 `test_resize.cpp` 3 段：grip 拖调（down@95,75 + move +10,+10 → 110×90）+ maximize（fill 300×200）+ restore（回 110×90）+ minimize（visible false/true）。
- standalone ctest **22/22** + ASAN 干净。
- Host ABI 零改动；core 仍 host-neutral。

## CinuxOS 侧

零改动。

## 下一步

P6-c 终端 ANSI bg(40-47) + 256 色(38;5;N) + 光标块。
