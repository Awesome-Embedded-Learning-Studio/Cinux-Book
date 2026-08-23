# P6-d — Scene 退役 + Compositor handler 表 OCP

> P6 第四批（收尾）。P2 的 Scene + Scene-based Compositor 退役；Compositor 重做成**类 + handler 表**（加图元 OCP，不炸 render）。

## 背景

两个问题：
1. **Scene 老 P2 路径**：core/scene.* + compose(Scene) + Compositor(compose(Scene) 帧间 diff) + offscreen/replay host + test_scene/test_compositor。控件树（P3+）+ per-widget dirty（P5-f）已覆盖等价能力，Scene 成债。
2. **Compositor 是自由函数 execute + switch(cmd.kind)**：每加一个图元（circle / line / image）都要改 render 的 switch —— 违反 OCP，用户骂"加图元合成器就炸"。

## 目标

删 Scene 全家 + Compositor 重做：**类**（Compositor::render）+ **handler 表**（CmdKind → fn，加图元 set_handler，render 不动）。Host ABI 零改动。

## 设计

- **删**：core/scene.hpp/cpp + compositor 的 compose(Scene)/Compositor(compose(Scene))/paint_scene_clipped/compose_window/diff_scene/... + host/offscreen_host_main.cpp + replay_host_main.cpp + test/test_scene.cpp + test_compositor.cpp。
- **Compositor 类**（compositor.hpp/cpp）：
  - `Handler = void(*)(Surface&, const PaintCmd&, const PsfFont&, const ClipRect*)`。
  - `handlers_[kKindCount]`（CmdKind → fn 数组）。
  - 构造注册默认图元（h_fill_rect/h_fill_round/h_text/h_text_glyph/h_text_scaled）。
  - `set_handler(CmdKind, Handler)`：加/override 图元（OCP）。
  - `render(list)`：遍历 cmd，kClipPush/Pop 管 clip 栈（内部），其他查 handlers_ 调。
- **Desktop.render**：`Compositor comp;` 局部 + `comp.render(staging, list, font, &clip)`（per dirty rect）。
- **test_font_scale**：Compositor 替 free execute。

## 决策

1. **handler 表（OCP）而非 switch**：render 只遍历 + 查表分发；加图元 = CmdKind + PaintCmd 字段 + swraster 原语 + set_handler，render **永不改**。这是用户两个反馈（类 + 不炸）的正解。
2. **kClipPush/Pop 不走 handler**：clip 是渲染状态（栈），非可绘制图元。render 内部管（intersect base + outer）。
3. **Compositor 无状态（今天）**：handler 表是固定（构造注册）。类形态为未来状态（cursor footprint / 帧间 diff / GPU 路径）留扩展点，且 handler 表有 owner。
4. **offscreen/replay 删（非迁）**：Scene-based P0/P2 host，与控件层（widgets-dump + fbdev-host + window-test/window-manager-test/dirty-test）功能重复。迁控件 = 与 widgets-dump 重复；删清债。fbdev-host 保（Host ABI 缝 + 真显）。

## 陷阱

- **CMake Edit 2/3 首版没真删**（offscreen/replay 段保留 + 注释重复）：Edit old/new 写错（new 含 old）。重 Edit 删。教训：删整段时 new 留空或单行注释，别复制 old。
- **clangd 滞后**：Compositor 改后 clangd 报 comp 未声明（widget.cpp 已加 Compositor comp;）。信 gcc（build 绿）。

## 验证

- standalone ctest **19/19**（原 23 - offscreen/replay/scene/compositor 4）+ ASAN 干净。
- Scene 零残留（grep scene 无 core/host/test 引用，除注释）。
- Compositor handler OCP：render 无 switch（kClipPush/Pop + handlers_ 查表）。
- Host ABI 零改动；core 仍 host-neutral。

## CinuxOS 侧

零改动。

## P6 收尾

**P6 桌面功能增强全完成 ✅**（a-d）：
- P6-a TextBox + CheckBox + 键盘路由
- P6-b 窗口 resize + 最大化/最小化
- P6-c 终端 ANSI bg + 256 色 + 光标块
- P6-d Scene 退役 + Compositor handler OCP

下一步候选：CinuxOS 删 kernel/gui/（CinuxOS 梭哈后）/ P7 GPU / 更多控件（Radio/Menu）/ sdl-host 键盘 demo 补。
