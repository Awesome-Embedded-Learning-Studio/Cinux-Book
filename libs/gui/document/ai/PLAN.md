# Cinux-GUI PLAN — 当前焦点批级进度

> 批级（最易变）。**里程碑全树见 [ROADMAP.md](ROADMAP.md)。**
> **P0 Probe-0 完成 ✅**：核心渲染管线 standalone 跑通——pump→core.staging→swraster→glyph_blit→flush，事件回放驱动拖拽，脏区纪律断言（idle 0 / 首帧全屏 / 后续局部）。3 个 ctest（smoke + offscreen + replay）全绿 + ASAN 干净。
> **P1 Probe-1 完成 ✅**：fbdev+evdev 真 host（`host/linux_fbdev_main.cpp`）+ evdev accumulator 单测（`test/test_evdev.cpp`，4 ctest 绿 + ASAN）+ **真 QEMU 冒烟 PASS**（自构建 x86_64 kernel + busybox initramfs，`fb0:bochs-drmdrmfb` + usb-tablet + fbdev-host 跑满 40s 不崩，像素采样验场景画对）。core 一行没动，只换 host 表——解耦缝证毕。详见 `document/notes/2026-06-30-p1-probe1-fbdev-host.md`。
> **P2 渲染收敛核心 ✅ 完成**：host-neutral **Scene（数据）+ Compositor（渲染器）** 收敛三 host 重复画法 + frame-to-frame dirty diff（只 composite 变化区，省 CPU 不止省 flush）。三 host 已切 Compositor，fbdev QEMU 冒烟 PASS（像素眼检无误）。**Host ABI 零改动**（CinuxOS 不 bump pin）。4 批 a/b/c/d 详见批表 + 笔记 `notes/2026-07-01-p2-*.md`。
> **P3 控件工具箱 ✅ 完成**：Widget 树 + PaintList + 事件路由 + Material flat Theme（圆角/配色）+ Label/Button/Slider/Container/HBox/VBox + press capture；widgets-host（SDL2）demo Material 端到端（ctest + 像素眼检）。**Host ABI 零改动**。4 批 a/b/c/d 见下 + 笔记。
> **P4 桌面迁入 🔄 启动中**：路 A——本仓 P3 控件框架上**重建** CinuxOS WM/Window/Terminal 桌面语义（CinuxOS 代码作参考、不搬不引）；terminal IO 走 PTY fork+exec；CinuxOS **暂不删** `kernel/gui/`（只 pin 新本仓，删码留后续）。批表见下。

## 现状速览（我们站在哪）

### 已有的（本仓 `core/`）

- **Host ABI 表**（`core/host.hpp`）——唯一硬边界：`HostCore`（flush / flush_complete / poll_event / dispatch_event / render_frame / now_ms / alloc / free / log）+ `HostDesktop`（spawn，可选）+ `Frame` 脏帧契约 + `PixelFormat{kXrgb8888, kArgb8888}`。回调全 NULL-check，部分填表也安全。
- **pump**（`core/pump.*`，~75 行）——排空 `poll_event` → `dispatch_event` → `render_frame` → 按脏 rect 逐个 `flush`；idle 帧 0 写屏。`kMaxDirtyRects=64`。
- **region**（`core/region.*`）——Rect 代数 intersect / union / subtract（4-strip）/ translate，`kMaxRects=32`，**容量溢出坍缩包围盒，永不欠覆盖**。
- **swraster**（`core/swraster.*`）——5 个纯 CPU 整数原语（fill_rect / blit / blit_blend Q8.8 定点 / glyph_blit 1-bpp mask / draw_line Bresenham），仅 XRGB8888。
- **event ABI**（`core/event*.hpp`）——定长、版本化：`EventHeader` 8B + `PointerPayload`(18B) / `KeycodePayload`(3B)。`kEncoder`/`kTouch` 留位未定义。
- **abi_check + fake_host_main**——编译期布局自检 + host 中立性证明（standalone ctest 绿）。

### 关键的「没有」（本仓）

- 没有控件 / WM / 合成器 / SDL/X11/Wayland 真 host（README 全 TODO）。→ **P3 控件工具箱已落地**（Widget/Label/Button/Slider/Container/HBox/VBox + Material）；**SDL2 交互 host**（`sdl-host`,opt-in `-DCINUX_HOST_SDL=ON`,host 层 core 零 SDL）+ `demo-dump`（丰富 Material PPM）已加；X11/Wayland/WM 仍 TODO。
- ~~**swraster 没接进 pump**~~ → **P0-a 已接进渲染路径**：core 拥有 staging Surface（新增 `GuiCore`），pump 预填 staging → host 经 `render_frame` 画进去 + 报脏区 → region 收脏 → flush。但 **host 还没画真场景**（窗口/光标/文字 = P0-b 的 offscreen host），目前 fake_host 只验证所有权链路。

### CinuxOS 侧（不在本仓，但要知道）

真正在跑的「桌面」住内核 `kernel/gui/`（12 TU：WM / terminal / desktop / host_cinux 适配器 / SPSC 事件队列），run-kernel-test **928/0** + GUI 冒烟零 panic，能拖拽 / Z 序 / 多终端跑真 musl shell / 脏区 flush / usb-tablet 绝对鼠标。**好使，但跟内核 API（Canvas / Pipes / TaskBuilder / launch_user_program）缠绕。**

## 当前焦点：P0 探针 Probe-0

**目标**：证明核心（pump + swraster + region）能画真像素、被输入驱动、standalone 跑（零 CinuxOS 依赖）。最小但端到端。

### 前置必做（绕不开）：把 swraster 接进 pump（visor-02 §4c）

改 pump 渲染路径：核心拥有一块 staging `Surface` → swraster 把场景画进去 → region 算脏区 → 脏 rect 经 `flush` 推 host。第一步先标准化「staging Surface + 脏区」契约——`render_frame` 给 host 一块 `Surface` + 当前脏区，host 把场景画进这块 Surface，pump 算脏 + flush。

### 两级探针（先 0 后 1）

| 级 | 形态 | 输入 | 显示 | 跑哪 |
|---|---|---|---|---|
| **Probe-0** | 无头 / 离屏，**终极可调试** | 极简文本回放（`+100 move 100 200` / `+200 down`） | malloc 影子缓冲 → dump PNG | WSL2 / 任何 Linux，零硬件 |
| **Probe-1** | 真 fbdev + evdev | `/dev/input/event*` | `/dev/fb0` mmap | QEMU（现成 Alpine 镜像 + VNC，**不编译 Linux**）/ 带显示真机 |

- **Probe-0 是验证基建本身**：确定性回放 + dump 帧 = 金帧对比 + 脏区断言。WSL2 直接跑，出 bug 最易定位。
- **Probe-1 证明第二个真 host**：跟 Cinux 同海拔，证明解耦缝干净。现成镜像，绝不 buildroot。
- **边际**：Cinux 自己就是「真 fbdev 级」host。Probe-0 验稳后，Probe-1 独有价值仅是「第二个真 host 证缝」，可择机。

### 最小场景

带边框的窗口 + 可用鼠标/回放事件拖动的光标 + 文字（内核 PSF 字体搬进本仓，`glyph_blit` 渲个 "Hello"，管线通没通一眼明白）。

### 验收

- Probe-0：回放事件，dump 的帧序列光标/窗口动、脏区只刷该刷的 rect（非全屏）；金帧对比过。
- Probe-1：QEMU 真鼠标动，屏幕光标跟动；不崩、fbdev 打开、事件读到。
- **全程零 CinuxOS 依赖**——本仓 standalone 构建 + 跑。

## 批表（P0）

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P0-a** | staging Surface 契约 + swraster 接进 pump（`render_frame` 改画 staging + region 算脏） | ✅ | `9e71e3f` | ctest 绿 + ASAN 干净 |
| **P0-b1** | 字体基建：PSF2 字体搬进本仓（constexpr C 数组）+ 解析器 + glyph_blit 渲字单测 | 🔜 NEXT | — | ctest 绿（parse 8×16/256 + 'A' 非空） |
| **P0-b2** | offscreen host + PPM dump + 静态场景（窗口 + 标题栏 + 多行文字 + 光标）一帧 | ✅ | `f08abbf` | ctest 绿 + ASAN + 视觉确认 |
| **P0-b3** | 事件回放（文本→Event）+ 拖拽交互 + 多帧 dump + 金帧/脏区断言 | ✅ | `f4c95ca` | ctest 绿 + ASAN + 几何/脏区断言 |
| **P0-c** | 金帧 / 脏区断言进 ctest + host 单测 ASAN 自验 | ✅ 被吸收 | — | 脏区断言进 ctest（b3）+ 金帧用结构断言代替二进制 fixture（更鲁棒）+ 每批 ASAN |

## 批表（P2 渲染收敛核心）

> 决策点已定（用户拍板）：① Scene/Compositor 放 `core/`（host-neutral 库本位；GuiCore「对场景无知」指 pump 驱动器，非禁 core/ 有 Scene 类型）② `draw_text`/`draw_rect_outline` 留 Compositor 内部（保 swraster 纯原语层，不反向依赖 font）③ 帧间 dirty 由 Compositor 持 prev Scene 处理（窗口移动露背景 = old_rect∪new_rect 进 Region）。**Host ABI 零改动，CinuxOS 不 bump pin**。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P2-a** | Scene 数据模型（`core/scene.*`：Window 栈 + Cursor + 文本 run，纯 POD，零 swraster/host include） | ✅ | `2fe37af` | 新 `test_scene.cpp`：构造/几何/截断/容量断言绿 + ASAN |
| **P2-b** | Compositor 接管绘制（`core/compositor.*`：`compose(staging, scene)`，收敛三 host 重复 paint） | ✅ | `f18414e` | 新 `test_compositor.cpp`：像素结构 == 旧 offscreen（零回归） |
| **P2-c** | 帧间 dirty diff（Compositor 持 prev Scene，只 composite 变化区，返回 dirty Region） | ✅ | `4310e64` | compositor-test dirty 段：首帧全屏 / idle / cursor 移 / window 移（+露 bg）/ bg 变 |
| **P2-d** | 三 host 切 Compositor + fbdev QEMU 冒烟验无回归 | ✅ | `59aa964` | ctest 全绿 + ASAN + QEMU 冒烟 PASS（像素眼检无误） |

## 批表（P3 控件工具箱 · Material 风格）✅

> 决策点已定（用户拍板）：① **控件树 PaintList 取代 P2 Scene** 作场景源（干净但要迁移三 host，长痛不如短痛）② **flat Material 首批**：配色 + 圆角 + 色差层次（不用 blur 阴影）；**不做**波纹/动画/过渡（纯整数算不起，留 P5+ 动画系统）③ 布局 HBox/VBox 线性首批（padding/margin/align，不做 flex）；事件 hit-test **点谁谁处理**（不冒泡）。**Host ABI 零改动，CinuxOS 不 bump pin**。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P3-a** | 控件模型 + PaintList + 事件路由：Widget 基类（rect/children/hit_test/on_pointer/paint_to_list）+ PaintList（原语序列 fill_rect/text/clip）+ Desktop（dispatch + render）+ Compositor::execute(PaintList) | ✅ | `ef11d19` | 新 test_widget：hit-test 命中 + dispatch + PaintList→像素 |
| **P3-b** | swraster 圆角 + Material Theme：`fill_rounded_rect`（整数 isqrt 逐行角弧）+ `core/theme.*`（Material 配色 primary/surface/on-surface + 圆角半径 + 8dp 网格）+ execute 转真圆角 | ✅ | `b89d8fb` | 圆角像素（角不溢出）+ theme 配色 + radius0/clamp |
| **P3-c** | 基础控件 + 布局：Label / Button（down/hover/press 状态）/ Container + HBox/VBox + Widget::layout hook | ✅ | `d1729ba` | widgets-test：Label/Button rest-press/HBox/VBox 几何/desktop |
| **P3-d** | Slider + Desktop press capture + widgets demo host（Slider 拖动取值 + 圆 thumb；press capture 拖动跟手；widgets-host Material 控件树端到端 ctest + 像素眼检） | ✅ | `30a8114`+`137e5bc` | slider-test + widgets-dump（ctest 11/11 + ASAN + 像素眼检 Material） |

## 批表（P4 桌面迁入 · 路 A Widget 重建）🔄

> 决策点已定（用户拍板）：① **路 A**——本仓 P3 控件框架上**重建** CinuxOS WM/Window/Terminal 桌面语义（Window=复合 Widget：标题栏 HBox[Label + 关闭 Button] + 内容 Container；WM=Desktop 上层管 Z 序/click-to-raise/拖动/关闭）；CinuxOS `kernel/gui/` 代码仅作设计参考、**不搬不引**（其 Canvas-based 模型与 P3 Widget 框架两套世界，搬来长期冲突）。② **terminal IO = PTY fork+exec**：Linux host fork `/bin/sh` + 伪终端（raw/curses 可用），**首实装 `HostDesktop::spawn`**。③ **CinuxOS 暂不删 `kernel/gui/`**——P4 全在本仓重建，CinuxOS 仅在 Host ABI 变更时 bump pin；旧 kernel/gui/ 留对照，删码留后续里程碑（零删除风险，P4 纯加法）。④ dirty 先沿用 P3-a **全屏**（per-widget dirty flags + cursor footprint 留优化批），cursor 带简单软件光标。**Host ABI 目标零改动**（P4-d 首实装已定义的 spawn 回调，若 PTY 需扩签名才 bump pin）。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P4-a** | `Window` 复合控件（标题栏自画：色带 + 标题文字 + 关闭键；圆角 body + content 子控件）+ 标题栏拖拽 + 关闭回调（hit_test/on_pointer 自管，非 HBox —— P3 HBox 等分限制） | ✅ | — | 新 test_window：标题栏 hit-test / 几何 / 拖拽 / 关闭（6 断言绿） |
| **P4-b** | `WindowManager`（Z 序栈 / click-to-raise / 标题栏拖动移窗 / 关闭销毁）住 Desktop 上层 + 简单软件 cursor | ✅ | — | wm-test：Z 序 / raise / click-to-raise / close / cursor（6 断言绿） |
| **P4-c** | `TerminalWidget`（80×25 字符缓冲 + cursor + 换行/滚动 + 渲染到 PaintList），IO 先注桩（不接 shell） | ✅ | — | terminal-test：写字 / cursor / `\n`/`\r`/`\b` / 滚动 / 像素（6 断言绿）；扩 PaintList 4096 + 新 kTextGlyph cmd |
| **P4-d** | Terminal IO + 首实装 `HostDesktop::spawn`（Linux host forkpty + exec `/bin/sh` + PTY）+ terminal-host 真 shell 交互 | ✅ | — | posix-spawn-test（forkpty + echo）ctest + terminal-host 编译链接（真 shell 眼检留 WSLg） |
| **P4-e** | fbdev host 切控件树（`linux_fbdev_main` 经 WM+Window+Desktop.render 替 P2 Scene+Compositor）+ Scene **保留**（offscreen/replay 作 P0/P2 回归基线 + P2-c 帧间 diff 脏区纪律验证，控件树 P3-a 全屏 dirty 无此纪律 → 不退役） | ✅ | — | ctest 15/15 零回归 + fbdev-host 编译链接（QEMU 真 evdev→WM 冒烟手动留） |

### Follow-up：Scene 退役 / desktop icon / per-widget dirty（P4 后，迟早做）

- **Scene 退役**：P5-f per-widget dirty 落地（控件树有等价帧间 diff）→ 前提达成。剩 offscreen/replay host 迁控件树后即可删 Scene（仍保留作 P0/P2 回归基线 + 脏区纪律验证）。
- **desktop icon**：P4-e 暂跳（低价值装饰），留后续。
- **per-widget dirty**：✅ **P5-f 落地**（collect/per-row + execute-clip + host per-rect upload；Window move old+new 露背景已含）。控件树有等价帧间 diff → Scene 退役前提达成（仅剩 offscreen/replay 迁移）。
- **QEMU 真 evdev→WM 冒烟**：fbdev-host widget-tree edition 手动冒烟（`timeout 40 ./fbdev-host`，自构建 kernel+initramfs，VNC 眼检）。控件逻辑已被 window-test/window-manager-test 单测覆盖。

## 批表（P5 增强 · 字体/主题/dirty/flex/ANSI）🔄

> 决策点（用户拍板"四个方向全做"）：P5 覆盖字体/文本、主题、per-widget dirty、flex/per-corner 圆角、终端 ANSI 彩色。5 批 a-e，逐批定细节 + 实现。**Host ABI 目标零改动**（纯 core/控件层增强，CinuxOS 不 bump pin）。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P5-a** | 字体增强：swraster 整数缩放渲染（glyph_blit_scaled）+ 文本测量（text_width/height）+ Label set_scale（PSF 8×16 → 16×32，无新字体数据） | ✅ | — | font-scale-test（scale 2 = 4× pixel + 测量 + execute 一致） |
| **P5-b** | 主题配色变体（primary_container/on_primary_container/surface_variant，M3 值）+ sdl-host 按 T 运行时切 light/dark | ✅ | — | theme-test 段 6（变体断言）+ sdl-host 编译（T 键眼检） |
| **P5-c** | per-widget dirty（invalidate 传播 root）+ Desktop.render idle（root 不脏 → 0 rect 0 flush）；set_rect 不 invalidate（layout 内部），控件状态变化显式 invalidate。局部 dirty（只刷脏区）留后续 | ✅ | — | dirty-test（idle 0 + 交互 dirty）+ ctest 17/17 零回归 |
| **P5-d** | HBox/VBox flex 权重（set_flex，last 拿余数）+ swraster per-corner 圆角（kCorner 位掩码）+ Window 标题栏顶圆（TL+TR） | ✅ | — | flex-test（3:1/等分/per-corner 像素）+ widgets-test 适配 |
| **P5-e** | 终端 ANSI 彩色渲染（SGR 16 色 fg/bg + 光标定位/清屏执行，TerminalWidget cells 加 color 属性） | ✅ | — | terminal-ansi-test（SGR/光标/清屏 5 断言） |
| **P5-f** | per-widget 局部 dirty（P5-c idle 之上深一步）：Widget dirty_rect_+collect_dirty+invalidate(rect) / TerminalWidget per-row / Desktop.render collect+execute clip 脏区 / compositor execute outer clip / terminal-host per-rect SDL_UpdateTexture（砍 upload 量 90%+） | ✅ | — | ctest 19/19 + ASAN（terminal-host 性能眼检） |

## 批表（P6 桌面功能增强 · 控件/窗口/终端/Scene）🔄

> 决策点（用户拍板"顺序梭哈"）：P6 四方向全做 —— 控件(TextBox/Checkbox)+ 窗口 resize + 终端 ANSI bg/256 + Scene 退役。4 批 a-d，逐批实现。**Host ABI 目标零改动**（CinuxOS 不 bump pin）。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P6-a** | TextBox（键盘输入 + 光标）+ Checkbox(toggle) + Widget on_key/Desktop dispatch_key+focus 键盘路由 | ✅ | — | textbox-test + checkbox-test（ctest 21/21 + ASAN）；sdl-host 键盘 demo 留眼检 |
| **P6-b** | 窗口 resize（右下角 grip 拖调大小，min 40×24）+ 最大化/还原（set_maximized 存 prev）+ 最小化（set_minimized hide） | ✅ | — | resize-test（拖调 + maximize 还原 + minimize hide） |
| **P6-c** | 终端 ANSI bg(40-47/100-107) + 256 色(38;5;N fg / 48;5;N bg) + 光标块(visible cursor) | ✅ | — | terminal-bg256-test（bg/256/reset/cursor block） |
| **P6-d** | Scene 退役：删 core/scene.* + compose(Scene)/旧 Compositor(Scene) + offscreen/replay host + test_scene/test_compositor；Compositor 重做 **handler 表 OCP**（class + set_handler，加图元不改 render） | ✅ | — | ctest 19/19 + ASAN（Scene 零残留，Compositor handler OCP） |

## 批表（P7 控件扩展 + sdl demo + Compositor 状态）🔄

> 决策点（用户拍板"3+4 一起"）：Radio/Dropdown 控件 + sdl-host 键盘 demo（TextBox 端到端）+ Compositor cursor footprint（光标渲染进合成器类，handler 表铺的路用上）。**Host ABI 目标零改动**。

| 批 | 范围 | 状态 | commit | 测试 |
|---|---|---|---|---|
| **P7-a** | Radio + RadioGroup（非 Widget 单选管理器）+ Dropdown（rect 全列表高，closed 顶行/expanded 全 + 高亮） | ✅ | — | radio-test（互斥）+ dropdown-test（open/select/close）ctest 21/21 |
| **P7-b** | sdl-host 补键盘路由（SDL_TEXTINPUT/BACKSPACE → dispatch_key）+ TextBox/Radio/Dropdown demo（手动布局，per-rect upload） | ✅ | — | sdl-host 编译过（WSLg 眼检：键盘输入/单选/下拉） |
| **P7-c** | Compositor cursor 状态：cursor 渲染从 WM paint 移进 Compositor（cursor 成员 + set_cursor + render 末画，受 dirty rect clip）；Widget cursor_pos virtual + WM override；Desktop comp_ 成员（跨帧）；WM paint 去 cursor | ✅ | — | cursor-test（Compositor cursor 像素）+ ctest 22/22 + ASAN |

## 验证哲学

- **能确定的，全部自动化进本仓 ctest**：渲染对不对 = 金帧对比；脏区对不对 = 断言 `flush` 了哪些 rect；崩不崩 / ASAN / UBSAN = 自动。
- **不能确定的，别假装能自动化**：「好看 / 跟手」老实人工眼检。金帧 + 结构断言 + 人工看，这个组合。
- **不占 CinuxOS CI**：本仓 standalone ctest。bump pin 回 Cinux 时，再用 `make run` GUI 冒烟 + `run-kernel-test-all` 防回归。
