# 2026-07-01 · P2-d · 三 host 切 Compositor(收敛兑现)+ fbdev QEMU 冒烟

> P2 收官批。P2-a/b/c 立了 Scene 数据 + Compositor 渲染 + 帧间 dirty diff,但三 host 还各自手写 paint。本批把 offscreen/replay/fbdev 的 `render_frame` 全部切到「Scene + Compositor」,删掉重复画法,fbdev 重跑自构建 kernel+initramfs QEMU 冒烟验像素无回归。P2 至此合龙。

## 背景

切之前,三 host 的 `render_frame` 各自手写同一套 `paint_scene`(bg + window + titlebar + border + text + cursor)+ `draw_rect_outline` + `draw_text`(replay 注释自承「mirror offscreen」);replay/fbdev 还各自手写 `prev_cx/prev_cy/prev_wx/prev_wy` 帧间脏区逻辑。P2-a/b/c 把这些能力搬进了 host-neutral 的 `core/scene.*` + `core/compositor.*`,本批让三 host 真正用上。

## 目标

1. **三 host `render_frame` 切 Compositor**:从「手画 + 手算脏区」→「sync 位置进 Scene → `Compositor::compose(staging, scene, font, &dirty)` → 写 dirty rects 进 Frame」。
2. **删重复**:三 host 的 `paint_scene`/`draw_rect_outline`/`draw_text` + replay/fbdev 的 `prev_*/first_frame`/`cursor_rect`/`win_rect` 全退役(收敛进 core)。
3. **零回归**:replay 的 dirty 纪律断言(首帧全屏 / 后续局部 / idle 0)全过;fbdev QEMU 冒烟 PASS + 像素眼检。

## 设计

### HostState:加 Scene + Compositor,删 prev_*

- replay/fbdev 的 `HostState` 加 `Scene scene` + `Compositor comp`,删 `prev_cx/prev_cy/prev_wx/prev_wy/first_frame`(Compositor 内部持 `prev_` + `first_`)。
- `render_frame` 三步:① 把 host 的 cursor/win 位置 sync 进 `scene`(配色/文本常驻不变)② `comp.compose(s, scene, font, &dirty)` 画 + diff ③ 把 `dirty` 的 rects 写进 `Frame`(count=0 即 idle,pump 不 flush)。
- `make_scene(wx,wy,cx,cy)` 在 main 构初始 Scene(window + 文本 + cursor 的配色/文本一次性设好)。
- offscreen 是**静态**场景(无交互),用 P2-b 的 stateless `compose()`(不必持状态);replay/fbdev 是**交互**,用 stateful `Compositor`。

### 共享画核,paint 不再重复

三 host 不再 `#include` swraster 原语去拼场景;paint 全归 `core/compositor` 的 `paint_scene_clipped`。host 只剩「设备 I/O + 状态机 + sync 进 Scene」——这正是 Host ABI 缝该有的样子:host 管传输,core 管画。

## 决策

- **offscreen stateless / replay+fbdev stateful**:offscreen 一帧静态、无输入,用 stateless `compose()` 最简(也作 P2-b 零回归基线锚点);replay/fbdev 有交互、需帧间 dirty,用 `Compositor`。不必为一致强求 offscreen 也 stateful。
- **`HostState st{..., {}, {}}` 显式默认 scene/comp**:fbdev 的 HostState 有引用成员(fb/ev/font),aggregate init 到 `dragging` 后,`scene`/`comp` 未显式列 → `-Wmissing-field-initializers` 警告。加 `{}, {}` 显式默认构造(它们本就类内 `{}` 初始化),零行为变化,消警告(守 CODING-TASTE §12 零 warning)。replay 的 HostState 无引用成员(全默认构造,`HostState st;`),无此问题。

## 陷阱

- **IDE clangd 滞后(再再复发)**:三 host 加 `compositor.hpp`/`scene.hpp` include 后,clangd 沿旧 compile_commands 报「compositor.hpp file not found / Unknown type Scene/Compositor」一片。`cmake --build` 实际通过、ctest 6/6 绿。p0-a/P2-b/P2-c 笔记都记过,**以实际编译为准**。
- **5999 VNC 端口被占**:首次跑 smoke_p1.sh 报「Failed to find an available port」——上次 QEMU 残留进程占着 5999(MEMORY 约定 :99=5999)。`fuser -k 5999/tcp` 清掉(PID 97422)后重跑 PASS。冒烟脚本 trap EXIT 清理,但被 `timeout` 外杀时可能留残。
- **静态 build 的 `-Wmissing-field-initializers`**:standalone ctest build(default flags)不报,但 `build_fbdev_host_static.sh` 的 g++ 默认报。两种 build 都得过 → 零 warning 纪律对**所有** build 形态生效,不只 ctest。

## 验证

- **standalone ctest 全绿**(Release):6/6——offscreen-dump + replay-dump(像素 + dirty 纪律断言不变,证 Compositor 语义零回归)+ compositor-test/scene-test/evdev-test/cinux-gui-smoke。
- **ASAN 干净**:`offscreen-dump`/`replay-dump`/`compositor-test` 直接运行 exit 0。
- **fbdev QEMU 冒烟 PASS**(自构建 x86_64 kernel + busybox+fbdev-host initramfs):
  - `GUEST_FB0_OK name=bochs-drmdrmfb`、`GUEST_TABLET_EVENT=event3`、`GUEST_RUN_RC=143`(timeout 40s SIGTERM,正常退出非崩溃)。
  - 无 `Segfault/Bus error/kernel BUG/Oops`。
- **像素眼检**(`build/smoke/run/shot.ppm` → png):深海军蓝背景 + 矩形窗口(蓝色标题栏「Cinux」+ 浅灰体 + 清晰边框)+ 「Hello / Cinux-GUI」两行文字 + 白色小光标,**无空白/乱码**。与 P1 同场景像素一致 = 切 Compositor 零回归实证。

## 后续 / P2 收官

- **P2 完成 ✅**:Scene(数据)+ Compositor(全画 + 帧间 dirty diff)收敛三 host 画法,fbdev 真跑像素无误。**Host ABI 零改动,CinuxOS 全程不 bump pin**。
- **Debt**:`font.cpp`/`swraster.*`/`gui_core.*` 的零星 clang-format 残留(P2-a 记过),后续触碰时顺手 format。
- **P3(🔜)**:控件工具箱(Button/Label/Slider/Container + 布局 + 事件路由),长在 Scene/Compositor 之上。
- CinuxOS 侧零改动。
