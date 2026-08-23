# P4-a — Window 复合控件（标题栏 + 拖拽 + 关闭）

> P4「桌面迁入」（路 A：Widget 重建）第一批。在本仓 P3 控件框架上立**窗口**控件地基 —— CinuxOS `kernel/gui/` 的 Window 是 Canvas-based 逐像素 back buffer，本仓**不搬**，用 Widget/PaintList 重建。CinuxOS 代码仅作设计参考。

## 背景

P4 决策（用户 propose 拍板）：路 A（本仓重建 WM/Window/Terminal，不搬 CinuxOS Canvas 代码）/ terminal IO 走 PTY / CinuxOS 暂不删 `kernel/gui/`。P4-a 立 Window 控件 —— 后续 WindowManager（P4-b）管多窗口 Z 序，Window 自己先要能：画自己（标题栏 + body）、标题栏可拖、关闭键可点。

## 目标

host-neutral 的 Window 复合控件，复用 P3 Widget/PaintList/Theme，自包含（单窗口拖拽 + 关闭回调），不碰 Host ABI。

## 设计

**Window : Widget**，持一个 `content_` 子控件（用户挂内容，如 P4-c 的 TerminalWidget）。

- **paint_to_list**：
  1. `fill_round_rect`(surface 底，card_radius)—— 整体圆角 body
  2. `fill_rect`(primary 标题带，`kTitleBarHeight` 高)—— 标题栏色带
  3. `text`(标题，on_primary，左 pad)
  4. 关闭键：`text` "x"（右上角）；armed(按下)时反色(fill_rect on_primary 底 + primary x)
- **layout**：`content_` 定在标题栏下方(`content_rect` = rect 减顶部 `kTitleBarHeight`)，递归 `content_->layout()`。
- **hit_test**(自定义，覆盖默认 last-to-first)：close 区→this / 标题栏其余→this(drag) / content→`content_->hit_test` / 空 content→this。
- **on_pointer**：down 在 close→armed；down 在标题栏→记 drag 起点 + 窗口原点；move 拖动→`move_to_`(新原点，改 rect + relayout)；up 在 close 且 armed→on_close 回调(双条件防误触)。拖拽靠 Desktop press capture(down 命中 Window→press_target=Window→move 持续投 Window)。

## 决策

1. **标题栏自画，不用 HBox[Label + Button]**：P3 HBox **等分** children（`container.hpp`），表达不了"标题占满 + 关闭键贴角"。Window 自画标题栏几何 + hit 区。P5 flex 布局后可改真 HBox。
2. **关闭键自管 hit，不用 P3 Button**：Window 要拦截标题栏拖拽，自定义 hit_test 把 close/title 都返回 this 最顺；armed 反色自画。P3 Button 是通用控件（独立 pressed/Theme），窗口关闭键作特殊命中区自管更内聚。
3. **override 访问**：`layout`/`hit_test`/`on_pointer` 保持 public（基类 Widget 是 public virtual，override 不降级 —— 直接调静态类型 Window 需 public）；`paint_to_list` 保持 protected。
4. **圆角妥协**：`fill_round_rect` 四角全圆，标题带矩形覆盖顶部 → "上方角、下圆角"。非 bug，P5 swraster 加 per-corner radius 时优化。

## 陷阱

- **override 降级访问**（编译错）：首版把 `layout`/`hit_test`/`on_pointer` 放 protected 段 → `w.layout()` / `w.hit_test()`(静态类型 Window)报 `protected within this context`。C++ override 不窄化基类 public 访问，但直接调按派生类访问控制判。修复：三 override 移回 public。
- **`move_to_` 要 relayout**：拖动改 Window rect 后必须 `layout()` 重算 `content_` 位置，否则 content 不跟随。

## 验证

- 新 `test_window.cpp` 6 段：几何(layout content_rect) / hit(title+close+content+outside) / 拖拽(Desktop dispatch +30,+20 → 窗口移 40,40，content 跟随) / close 触发 / close 移出不触发 / flatten 含 round+fill+text。
- standalone ctest **12/12** + ASAN 干净（全量 `-fsanitize=address` rebuild）。
- Host ABI 零改动（`core/host.hpp` 没碰）；core 仍 host-neutral（零 host include）；纯整数 + 定长 + 虚函数（非 RTTI）。

## CinuxOS 侧

零改动（P4 全在本仓；Host ABI 没动，CinuxOS 不 bump pin）。CinuxOS `kernel/gui/` 旧码暂留对照（P4 决策③）。

## 下一步

P4-b WindowManager：多窗口 Z 序栈 / click-to-raise / 标题栏拖动（Window 自带，WM 串多窗口）/ close 回调触发销毁 / 简单软件 cursor。仍 host-neutral，不碰 Host ABI。
