# P4-b — WindowManager（Z 序 / click-to-raise / 关闭销毁 / 软件 cursor）

> P4 第二批。在 P4-a Window 之上立**多窗口桌面管理器** —— CinuxOS `kernel/gui/window_manager.cpp` 的 Z 序合成在内核是 Canvas-based + `wm.dirty()` footprint；本仓重建为控件树上的 WindowManager。CinuxOS 代码作参考。

## 背景

P4-a 的 Window 单独可拖可关，但只在自己身上。桌面要管**多个窗口**：谁在顶（Z 序）、点了谁就把谁置顶（click-to-raise）、× 键关掉窗口、一个软件鼠标指针。这些是 WindowManager 的活，住 Desktop 上层。

## 目标

host-neutral 的 WindowManager：Z 序栈 + click-to-raise + close 销毁 + 软件 cursor，自带 press capture（拖拽用），不碰 Host ABI。

## 设计

**WindowManager : Widget**，作桌面 root。**不用 Widget 框架的 children_** —— 框架 flatten 是 self→children，无法表达"cursor 画在所有窗口之上"（paint_to_list 在 children 之前）。所以 WM 自管 `windows_[]` 数组，`paint_to_list` 里手动按正确顺序画：bg → windows（底→顶，逐个 `Window::flatten`）→ cursor。`children_` 留空。

- **Z 序**：`windows_[0]` 最底，`windows_[count-1]` 最顶（最后加的在顶）。
- **add_window**：push 到末尾（顶）+ 注册 `on_close → remove_window`（WM 作 close 目标，window 不自销毁，caller 拥有）。
- **raise(w)**：把 w 移到末尾（顶）。
- **remove_window(w)**：数组前移、`count--`；若 w 是 press_target，清掉。**不 delete**（caller 拥有）—— 等价"隐藏 + 移出 Z 序"。
- **hit_test**：从顶（末）到底，第一个 `rect().contains` 的窗口 → 返回该 **Window**（不深入 content）：P4 窗口内容无 pointer 交互控件，WM 要拿 Window 才能驱动 raise/drag/close。
- **process_pointer**（自带 press capture）：更新 cursor pos；down → hit → raise（click-to-raise）→ 投给 window（arm close / begin drag）→ 设 press_target；move → 投 press_target（拖）；up → 投 press_target（可能触发 on_close → remove）→ 清。
- **paint_to_list**：`fill_rect`(bg) → 逐窗口 `flatten`（底→顶）→ `fill_rect`(cursor，4×4 白方块)。

## 决策

1. **不用框架 children_，自管 windows_**：框架 flatten 顺序（self→children）画不了"cursor 在窗口之上"。自管数组 + paint_to_list 手动画序，干净绕过。代价：WM 的 children_ 永远空（add_window 不 add_child）。
2. **hit_test 返回 Window（顶层），不深入 content**：P4 窗口内容（Label / 未来的 TerminalWidget）无 pointer 交互；深入会返回 content_，press_target 类型就不是 Window*。返回 Window 让 WM 统一驱动。将来 P5 若内容有交互控件，再深入。
3. **close = 隐藏 + 移出 Z 序（不 delete）**：caller 拥有 window 生命周期；WM 只管"是否在桌面"。与 P3 Widget 定长、不动态分配一致。
4. **cursor 画在 paint_to_list 末尾**：bg→windows→cursor 顺序保证 cursor 永远最顶。

## 陷阱

- **press_target 类型**：hit_test 返回 `Widget*`（实际是 Window*），press_target_ 是 `Window*`，需 `static_cast`。安全前提是 hit_test 只返回 Window / nullptr（设计保证）。
- **close 回调在 up 路径触发 remove**：`process_pointer(up)` → `Window::on_pointer` → `on_close` → `close_cb_` → `remove_window` 改 windows_ 数组。此时仍在 up 分支，紧接着 `press_target_=nullptr`。remove_window 也会清 press_target_（双重保险）。

## 验证

- 新 `test_window_manager.cpp` 6 段：add 堆叠 / raise / click-to-raise（遮窗暴露区按下 → raise）/ close（× 按下+松开 → count-- ）/ cursor 跟踪 / flatten 含 fill(bg+cursor)+window。
- standalone ctest **13/13** + ASAN 干净。
- Host ABI 零改动；core 仍 host-neutral；纯整数 + 定长 + 虚函数（非 RTTI）。

## CinuxOS 侧

零改动。CinuxOS `kernel/gui/window_manager.cpp`（Canvas-based + dirty footprint）暂留对照。

## 下一步

P4-c TerminalWidget：80×25 字符缓冲 + cursor + 换行/滚动 + 渲染到 PaintList（IO 先注桩，P4-d 接 PTY）。需扩 PaintList 容量（256 cmd 装不下字符密集的终端）。
