# P7-c — Compositor cursor 状态（cursor 移进合成器）

> P7 第三批（收尾）。cursor 渲染从 WM paint 移进 Compositor —— 光标本就是合成层职责。handler 表（P6-d）铺的路用上：Compositor 加状态（cursor）不动调用方签名。

## 背景

P4-b WM paint_to_list 末画 cursor（cursor 是 WM 的 paint 一部分）。但 cursor 是合成层职责（最顶、独立于 widget 树）。P7-c 把 cursor 移进 Compositor（状态进类），WM 只暴露 cursor 位置。

## 目标

Compositor 持 cursor 状态（x/y/visible）+ render 末画 cursor；WM 通过 `cursor_pos()` virtual 暴露位置 + 不再自己画；Desktop 持 Compositor 成员（跨帧 cursor）。Host ABI 零改动。

## 设计

- **Compositor cursor 成员**（compositor.hpp）：`cursor_x_/y_/visible_` + `set_cursor(x,y,visible)`。render 末 `fill_rect(4×4 白, cur())`（受当前 dirty rect clip —— per-rect render 只在 cursor 所在脏区画，不重复）。
- **Widget `cursor_pos(x,y)` virtual**（widget.hpp）：默认返 false（无 cursor）。WM override 返 `cursor_x_/y_ + cursor_visible_`。
- **Desktop `Compositor comp_` 成员**（widget.hpp）：render 读 `root_->cursor_pos` → `comp_.set_cursor` → `comp_.render`（per dirty rect）。comp_ 成员跨帧（cursor 状态持久）。
- **WM paint 去 cursor**（window_manager.cpp）：paint_to_list 不再画 cursor（Compositor 画）。

## 决策

1. **cursor 受 cur() clip（非 nullptr）**：Desktop.render per-rect（每 dirty rect 一次 render）。cursor 用 cur()（当前 dirty rect clip）→ 只在 cursor 所在脏区画。若 nullptr（无 clip），每 dirty rect 都画 cursor（重复，cursor 在多处）。cur() 正确。
2. **Widget cursor_pos virtual（基类）**：轻微泄漏（Widget 知 cursor 概念），但干净（Compositor 统一 cursor，WM override 暴露）。避免 Desktop 动态 cast WM（RTTI 禁）。
3. **Desktop comp_ 成员（非局部）**：cursor 状态跨帧（WM 设 cursor_pos，Desktop 读 → Compositor）。局部 Compositor（每帧 new）cursor 状态丢。comp_ 成员持久。
4. **WM paint 去 cursor**：cursor 单一来源（Compositor）。WM paint 只 bg + windows。

## 陷阱

- **fill_rect 缺 clip 参数**：cursor fill_rect 首版 6 参（缺 clip）→ clangd 报不匹配。fill_rect 7 参（+ clip）。加 cur()。
- **widget.cpp Desktop.render comp old 没匹配**（format 对齐）：Compositor comp 声明对齐空格。读精确后 Edit。
- **compositor.hpp 被 widget.hpp include**：clangd "not used directly"（Desktop comp_ 用 Compositor，clangd 滞后）。实际用（成员类型）。

## 验证

- 新 `test_cursor.cpp`：Compositor set_cursor(true) + render(空 list) → cursor 像素白；set_cursor(false) → 无。验 cursor 状态 + render 末画。
- standalone ctest **22/22** + ASAN 干净。
- WM cursor 经 cursor_pos → Compositor（window-manager-test 零回归：flatten 无 cursor 但 has_fill 仍真）。
- Host ABI 零改动；core 仍 host-neutral。

## CinuxOS 侧

零改动。

## P7 收尾

**P7 控件扩展 + sdl demo + Compositor 状态全完成 ✅**（a-c）：
- P7-a Radio/RadioGroup + Dropdown
- P7-b sdl-host 键盘 demo
- P7-c Compositor cursor 状态（cursor 移合成器）

下一步候选：CinuxOS 删 kernel/gui/ / P8 GPU / 更多控件（Menu）/ Compositor 更多状态（帧间 diff）。
