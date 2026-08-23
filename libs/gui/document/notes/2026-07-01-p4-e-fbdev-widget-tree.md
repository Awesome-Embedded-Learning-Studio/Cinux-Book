# P4-e — fbdev host 切控件树 + Scene 保留决策（P4 收尾）

> P4 第五批（收尾）。fbdev host 从 P2 Scene 路径切到 **P4 控件树**（WindowManager + Window），证真 host（fbdev+evdev）走与 SDL/terminal host 同一控件层。Scene **不退役**（决策，见下）。P4 全完成（a/b/c/d/e）。

## 背景

P4 控件层（Window/WM/Terminal）已在 widgets-host/terminal-host（standalone harness）证过。但 fbdev-host 是**经 Host ABI 表 + `GuiCore::pump`** 的真 host（证 Host ABI 缝），它还停在 P2 Scene + Compositor。P4-e 把它切到控件树，让真 host 也走控件层，Scene 路径从生产路径退到回归基线。

## 目标

fbdev-host（`linux_fbdev_main`）用 WindowManager + Window + Desktop.render 替代 Scene + Compositor。经 pump + Host ABI 表不变（仍证缝）。**Host ABI 零改动**。

## 设计

`linux_fbdev_main` 改动：
- **HostState**：`WindowManager wm; Window win; Label content; Desktop desktop;` 替 `Scene scene; Compositor comp;`（+ `Theme theme`）。
- **dispatch_event**：`wm.process_pointer(p)` 替手写 `in_title_bar` + drag 逻辑（WM 自带 drag/raise/close + cursor）。
- **render_frame**：`desktop.render(s, font, &dirty)` 替 `comp.compose(s, scene, font, &dirty)`。
- **make_scene → setup**：wm.set_rect(全屏) + win.set_rect(中心) + content(Label "Hello\nCinux-GUI") + wm.add_window + desktop.set_root(&wm)。
- poll_event / flush / make_host 不变（evdev→EventHeader / staging→fbdev blit）。

fbdev-host 是 **Host 表 host**（非 standalone）：`render_frame` 回调里调 `desktop.render` 画 core 拥的 staging。pump 调它。控件树作场景源，经 Host ABI 缝。

## 决策

1. **Scene 保留，不退役**：Scene 仍被 `offscreen-host`/`replay-host`（P0/P2 dump host）+ `test_scene`/`test_compositor` 引用。它们验证 **P2-c 帧间 diff 脏区纪律**（`Compositor::compose(Scene)` 持 prev Scene 算局部 dirty）。控件树是 **P3-a 全屏 dirty**（`Desktop::render` 每帧全屏），没有帧间 diff —— 迁过去就丢了那个验证。所以 Scene 留作 P0/P2 回归基线 + 脏区纪律载体，**退役推迟**（per-widget dirty + 帧间 diff 落地后再议，或永久作基线）。同 P3 决策②"Scene 保留作回归基线，不强删"。
2. **fbdev dirty 全屏**（P3-a `Desktop::render`）：QEMU 每帧全屏 blit，性能降于 P2-c 局部 dirty，但冒烟够（验控件，非性能）。per-widget dirty + 帧间 diff 留优化（PLAN P4 决策④）。
3. **desktop icon 跳过**：P4-e 原 PLAN 含 desktop icon constexpr 数据，但属装饰、低价值，留后续。P4-e 聚焦 fbdev 控件化（核心价值：真 host 走控件）。
4. **`scene::Window` vs `widget::Window` 命名冲突**（P4-d 已踩）：fbdev-host 控件化后只用 `widget::Window`，去 `scene`/`compositor` include → 无冲突。

## 陷阱

- **clangd 误报**：去 `compositor.hpp` 后 clangd 报一堆 unknown type（它没认 core/ include path）。gcc 编译用 cinux-gui PUBLIC include 找到。信 gcc（fbdev-host 编译链接通过）。
- **fbdev-host 非 standalone**：经 pump + Host 表（不像 sdl-host/terminal-host 直接 `desktop.render`）。`render_frame` 回调内调 `desktop.render` 画 staging。
- **HostState 聚合初始化**：含引用成员（fb/ev/font），C++17 aggregate `{fb, ev, font, theme, {}, {}, {}, {}}` 初始化（引用绑定 + 值拷贝 + 其余默认构造）。

## 验证

- standalone ctest **15/15** + ASAN 干净（零回归；fbdev-host 不进 ctest——手动 QEMU）。
- **fbdev-host 编译链接通过**（widget-tree edition）。
- **QEMU 真 evdev→WM 全链路冒烟**：手动（`timeout 40 ./fbdev-host`，自构建 kernel+initramfs，VNC 眼检拖窗/raise/cursor）。本环境未跑，留用户/PenguinLab 工具链。控件逻辑（WM/Window）已被 window-test/window-manager-test 单测覆盖。
- Host ABI 零改动（core/host.hpp 没碰）；CinuxOS 不 bump pin。

## CinuxOS 侧

零改动。CinuxOS `kernel/gui/`（Scene-based Canvas desktop）暂留对照（P4 决策③：暂不删，只 pin 新本仓）。

## P4 收尾

**P4 桌面迁入全完成 ✅**（路 A：Widget 重建）：
- P4-a Window 复合控件（标题栏/拖拽/关闭）
- P4-b WindowManager（Z 序/click-to-raise/关闭销毁/cursor）
- P4-c TerminalWidget（字符终端 + kTextGlyph/扩容 PaintList）
- P4-d terminal-host 真 shell（forkpty PTY）+ posix_spawn
- P4-e fbdev-host 切控件树（Scene 保留）

桌面层（WM/Window/Terminal）host-neutral 住本仓；CinuxOS `kernel/gui/` 旧码暂留对照（删码留后续里程碑）。Host ABI 全程零改动。下一步候选：per-widget dirty + 帧间 diff（→ Scene 可退役）/ ANSI 解析 / flex 布局 / 字体增强（P5）。
