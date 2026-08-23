# P5-c — per-widget dirty + Desktop idle（0-flush）

> P5 第三批。P3-a Desktop.render 每帧**全屏 dirty**（60fps 全刷，idle 也刷）。P5-c 加 per-widget dirty 标志 + idle 检测：无变化时 **0 rect → pump 0 flush**（省 idle CPU/带宽）。局部 dirty（只刷脏区）留后续。

## 背景

P3-a 简单稳（全屏 dirty），但 idle（屏幕静止）仍每帧全刷 fbdev/SDL —— 浪费。P4 terminal-host 跑 shell，无输出时画面静止，应 idle。P5-c 引入 dirty 标志 + idle 跳过。

## 目标

Widget dirty flag（invalidate 传播 root）+ Desktop.render idle（root 不脏 → 0 rect → 0 flush）。**Host ABI 零改动**。

## 设计

- **Widget 加 `dirty_`（初 true，首帧画）+ `parent_`（add_child 设）**。
  - `invalidate()`：dirty_=true + 传播 parent（→ root）。
  - `is_dirty()`：返回 dirty_（root 反映全树，因传播）。
  - `clear_dirty()`：dirty_=false + 递归 children。
- **Desktop.render idle**：`if (!root->is_dirty()) return;`（dirty 已 clear，0 rect → pump 0 flush）。脏则 clear_dirty + layout + flatten + execute + 报全屏 dirty。
- **set_rect 不 invalidate**：set_rect 是布局原语（layout 每帧调 content_->set_rect），若 invalidate 则每帧脏永不 idle。运行时 rect 变（Window move）由 `move_to_` 显式 invalidate。
- **控件状态变化显式 invalidate**（穷举）：
  - Button on_pointer（pressed 变）
  - Slider on_pointer（drag，value 变）
  - Window on_pointer（close armed / drag begin / up）+ move_to_（rect 变）
  - TerminalWidget write/clear（cells 变）
  - WindowManager process_pointer（cursor 移 / raise / close）+ add_window
- **运行时 set_\***（set_theme/set_color，如 sdl-host T 键）：调用者负责 `root->invalidate()`（sdl-host T 键加 root.invalidate）。

## 决策

1. **idle 优化（0 flush）而非局部 dirty（只刷脏区）**：idle 省（无变化不刷），变化时仍全屏重画。局部 dirty（per-rect execute，只刷脏控件区 + 露背景恢复）复杂（控件重叠/遮挡），留后续。P5-c 解决"idle 浪费"，不解决"变化时全屏"。
2. **set_rect 不 invalidate**：区分布局内部 set_rect（layout，每帧）vs 运行时 set_rect（move）。set_rect 不自动 invalidate；运行时变化点（move_to_）显式 invalidate。否则 layout 每帧 set_rect → 永不 idle。
3. **控件自驱 invalidate（交互/输出）+ 调用者驱 invalidate（运行时改属性）**：on_pointer/write 等自驱；set_theme/set_color 等（public，初始化/运行时都用）调用者 invalidate（sdl-host T 键 root.invalidate）。
4. **dirty_ 初 true**：首帧必画（控件构造未画）。clear_dirty 后 idle。

## 陷阱

- **漏 invalidate = stale**：某状态变化没 invalidate → 画面不更新（交互无响应）。穷举所有"屏幕可见变化"点。test_dirty 覆盖关键 + 手动眼检（sdl-host/terminal-host）。
- **set_rect 若 invalidate → 永不 idle**：layout 每帧 set_rect。set_rect 不 invalidate 是关键约束。
- **clear_dirty 时机**：render 开始（is_dirty 检查后）clear，layout/flatten 前。paint 只画（不 invalidate），安全。交互（dispatch，render 外）invalidate 下帧处理。
- **idle 仍全屏 dirty（变化时）**：变化时全屏重画（非局部）。局部 dirty 留后续（要 per-rect execute + 背景恢复）。

## 验证

- 新 `test_dirty.cpp`：首帧 dirty（add_window invalidate）→ 二帧 idle（0 rect）→ process_pointer（cursor）→ dirty → idle。**ctest 17/17 + ASAN 干净，零回归**（P5-c idle 改 render 核心，window/widgets/slider/window-manager/terminal-test 全过）。
- Host ABI 零改动；core 仍 host-neutral；纯整数 + 定长 + 虚函数。

## CinuxOS 侧

零改动。

## 下一步

P5-d HBox/VBox flex 权重 + swraster per-corner 圆角（Window 四角圆 + 标题栏可真 HBox）。
