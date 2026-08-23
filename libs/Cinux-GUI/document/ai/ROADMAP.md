# Cinux-GUI ROADMAP — 里程碑全树 + 状态

> 里程碑级（比 DIRECTIVES 易变，比 PLAN 稳）。**当前焦点批级进度见 [PLAN.md](PLAN.md)。**
> 架构背景（七层 + Host ABI）详见 CinuxOS `document/notes/2026-06-21-f13-visor-*.md`，以后择机迁入本仓。

## 定位

**Cinux-GUI = 完整、独立、跨平台的 GUI 系统**：核心（pump / swraster / region / event）+ 桌面层（WM / 终端 / 控件）+ 多 host（Linux 先，Cinux 内核适配器后）。**Host ABI 表（`core/host.hpp`）是唯一缝**——桌面层住表上面、对 host 无知，换 host = 换表填充，核心不变。CinuxOS 只剩一个薄 host 适配器。

## 战略方向（2026-06-30 用户拍板）

1. **极致独立**：GUI 所有细节尽可能进本仓，最大化解耦。本仓 = 完整 GUI 系统。
2. **Host ABI 唯一缝**：桌面层只调表里的 `flush` / `poll_event` / `now_ms` / `spawn`。
3. **CinuxOS 只剩薄 host 适配器**：`kernel/gui/` 的 WM/terminal/desktop 最终搬上表、住进本仓。**长期看 CinuxOS 是减代码。**
4. **Linux 先跑，依赖越底层越好**：fbdev（`/dev/fb0` mmap）+ evdev（`/dev/input/event*`），零外部依赖，跟 Cinux VBE+flush 同海拔 1:1 对照，好调试。SDL/X11/Wayland 是高层 host，以后再加，**不用于调试核心**。

## 里程碑树（本仓自有编号）

| 阶段 | 内容 | 状态 |
|---|---|---|
| **P0** | 探针 Probe-0：无头跑通核心渲染管线（swraster 接 pump + offscreen host + 事件回放 + 极简场景 + 金帧验证） | ✅ 完成（PR #2 合入 main）：3 ctest + ASAN，pump→staging→swraster→glyph→flush 端到端 |
| **P1** | 探针 Probe-1：真 fbdev + evdev（第二个真 host，证明解耦缝） | ✅ 完成：代码 + 单测（4 ctest + ASAN）+ 真 QEMU 冒烟 PASS（自构建 kernel+initramfs，`fb0:bochs-drmdrmfb`+usb-tablet+fbdev-host 跑满 40s 不崩，像素证据）；笔记 `notes/2026-06-30-p1-probe1-fbdev-host.md` |
| **P2** | 渲染收敛核心：swraster 正式接管，compositor dirty-region（只重画脏区，省 composite 本身） | ✅ 完成（4 批 a/b/c/d）：host-neutral Scene（数据）+ Compositor（全画 + 帧间 dirty diff）收敛三 host 画法；fbdev QEMU 冒烟 PASS + 像素眼检；Host ABI 零改动。笔记 `notes/2026-07-01-p2-*.md` |
| **P3** | 控件工具箱：Button / Label / Slider / Container + 布局 + 事件路由 | ✅ 完成（4 批 a/b/c/d）：Widget 树 + PaintList + 事件路由 + 圆角/Material flat Theme + Label/Button/Slider/Container/HBox/VBox + press capture; widgets-host demo Material 端到端（ctest + 像素眼检）。fbdev 控件化 + Scene 退役作 follow-up。Host ABI 零改动 |
| **P4** | 桌面迁入（路 A：Widget 重建）：本仓 P3 控件框架上重建 WM/Window/Terminal 桌面语义；terminal IO 走 PTY fork+exec；CinuxOS **暂不删** `kernel/gui/`（只 pin 新本仓，删码留后续） | ✅ 完成（5 批 a/b/c/d/e；ctest 15/15 + ASAN；Host ABI 零改动） |
| **P5** | 增强：字体/文本（缩放渲染 + 测量）+ 主题运行时切换 + per-widget dirty + flex 布局/per-corner 圆角 + 终端 ANSI 彩色渲染 | ✅ 完成（5 批 a-e；ctest 19/19 + ASAN；Host ABI 零改动） |
| **P6** | 桌面功能增强：TextBox/Checkbox 控件（+键盘路由）+ 窗口 resize/最大最小化 + 终端 ANSI bg/256 色 + Scene 退役 | 🔄 启动中（4 批 a-d） |
| **P7** | 控件扩展（Radio/Dropdown）+ sdl-host 键盘 demo + Compositor 状态（cursor footprint 进类） | 🔄 启动中（批 a-c） |
| **P8** | GPU texture compositor（原 P6/P7；有真 GPU 目标后；非 primitive draw-list） | 远期 |
| **P9** | 多进程 Surface 协议（原 P7/P8；attach/damage/commit/release；ring-3 桌面 server） | 远期 |
| **MCU 线** | visor 嵌入式（STM32F1 等）：独立 micro renderer，推迟到真板 RAM<20KB 实测后 | 长弧推迟 |

## 两仓契约（Cinux-GUI ↔ CinuxOS）

- **探针阶段（P0/P1）：CinuxOS 内核零改动。** CinuxOS 仍 pin `4c84eb2`，照常编 / 跑 / 过 `run-kernel-test`。
- **P2 起 Host ABI 若变**：CinuxOS bump pin + 改一处 `kernel/gui/host_cinux.cpp`（`render_frame` 改画 staging Surface）。**这是 CinuxOS 侧唯一对接点。**
- **P4 后（路 A）**：本仓重建 WM/Window/Terminal 桌面层（host-neutral）；CinuxOS `kernel/gui/` 旧码**暂留对照**（P4 仅本仓重建 + Host ABI 变更时 bump pin），后续里程碑再删 → 届时 CinuxOS GUI 净减少。
- **push / PR**：Claude 在 feature 分支 commit（绿才提交）；**push 与 PR 由用户控制**。

> **CinuxOS 侧 F13 文档待同步**（CinuxOS 在疯狂开发，不急，择机）：把 ROADMAP F13 行改成指向本仓；清理 `document/todo/README.md` 里 stale 的 3-milestone 模型；visor-02 §9 risk 表对齐 git 现状（PIT 反转等项已落地）。
