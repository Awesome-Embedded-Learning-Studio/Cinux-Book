---
title: Lab 054 · GUI 解耦与 visor 验证
---

# Lab 054 · GUI 解耦与 visor 验证

> 对应 `document/book/09-gui/054-gui-decouple-visor.md`。验证档 **B 档**(机制/重构,靠构建 + 测试 + grep 验证)。本章把 GUI 从内核里拔出来:核心几何/事件/光栅化逻辑外置成 host-neutral 的 `cinux-gui` 库,内核只留一个填 Host 表的适配单元,刷新从 PIT 中断挪到 worker 线程。验证核心三件事——core 真的不碰内核、它能脱离内核独立编译跑测试、内核侧 GUI 测试(含新增 region/dirty/swraster)全绿。

## 目标

确认五件事:

1. `cinux-gui` 的 core(`third_party/Cinux-GUI/core/`)**零内核 include**,是 host-neutral;
2. core 能**脱离内核独立编译**并跑通 smoke 测试(`cinux-gui-smoke`,不需要 QEMU);
3. 内核侧只有**一个** host adapter TU(`host_cinux.cpp`)在填 Host 表;
4. GUI 刷新由 `gui_worker` 线程的 `pump()` 驱动,PIT 回调已退役(`set_tick_callback` 无人调用);
5. `big_kernel_test` 构建 0 error + `run-kernel-test` 全绿,含本章新增的 region/dirty/swraster 用例。

## 步骤

### 1. core 不碰内核(host-neutral 的硬证据)

```bash
grep -rn '#include' third_party/Cinux-GUI/core/ \
  | grep -vE '<stdint|<stddef|<stdbool|<string\.h>' \
  | grep -vE '"(event|event_payload|host|region|swraster|pump|pixel|frame|abi)'
```

应**无输出**——core 只 include `<stdint.h>` 这类 freestanding 头和它自己的头,不认识 framebuffer、不认识 IRQ、不认识任何内核设施。它和外界唯一的接缝是 Host 表。

> **思考**:为什么用函数指针表(`Host`),不用 C++ 抽象基类的虚函数?——见章节:表是纯数据,不带 vtable/RTTI/异常的特权包袱,core 因此能保持 freestanding 友好、任意 hosted 编译器可编;表项允许 `nullptr` + `pump()` 逐项判空,所以**半填的表也安全**(不能 spawn 的宿主 `desktop` 填 nullptr)。

### 2. core 脱离内核独立编译 + smoke 测试

`cinux-gui` 的 `CMakeLists.txt` 有双身份:作为子目录时只提供静态库;作为顶层 root 时自带一个 `cinux-gui-smoke` 冒烟程序(手填一张假 Host 表跑 `pump()`)。后一种就是 host-neutral 的活证据:

```bash
cmake -S third_party/Cinux-GUI -B /tmp/cgui-build && \
cmake --build /tmp/cgui-build -j$(nproc) && \
(cd /tmp/cgui-build && ctest --output-on-failure)
```

应看到 `libcinux-gui.a` 编出来、`cinux-gui-smoke` 编出来、`100% tests passed`。这个 smoke 程序**不需要内核、不需要 QEMU**——驱动内核的那同一份 core,驱动一个无内核的假宿主只靠换一张表的填充。这正是「未来 SDL/X11/Wayland adapter 的种子」。

### 3. 内核侧:只有一个 host adapter 在填表

```bash
grep -n 'cinux_host_init\|g_cinux_host\.core\.' kernel/gui/host_cinux.cpp | head
```

去看 `cinux_host_init()`(`host_cinux.cpp:371`):逐行把 `poll_event`/`render_frame`/`flush`/`now_ms` 指向内核设施(鼠标队列、窗口管理器、VBE 帧缓冲、PIT uptime)。这就是「换宿主 = 换表填充」里内核这一侧的填充。再确认 `kernel/gui/CMakeLists.txt` 的注释:`kernel/gui/` 只保留这一个 host-specific TU。

### 4. 刷新已从 PIT 挪到 worker 线程

```bash
# GUI 现在的驱动循环:gui_worker 线程 pump()+yield()
grep -n 'pump(\|yield()' kernel/proc/init.cpp
# PIT 回调机制还在,但全树没人注册它 → 实际 no-op
grep -rn 'set_tick_callback' kernel/
```

`init.cpp:32` 的 `gui_worker_thread` 应是 `while (true) { pump(...); yield(); }`。`set_tick_callback` 的 grep 应**只在 `pit.hpp` 的声明 + `pit.cpp` 的定义处命中,没有任何调用点**,所以 `tick_callback_` 是 `nullptr`,`invoke_tick_callback()` 是 no-op——退役的是「注册」,不是「机制」。

再读 `kernel/drivers/pit/pit.cpp:91` 和 `:94`,确认 `invoke_tick_callback()`(GUI,现 no-op)和 `Scheduler::tick()`(调度)是**两条独立语句**——这是退役 GUI 回调不连带废掉时间片轮转的前提。

### 5. 内核构建 + 测试(CINUX_GUI=ON)

```bash
cmake --build build --target big_kernel_test -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test 2>&1 | tail -3
```

> **别** `cmake --build build`(ALL):host 单测有既有债(`test_ext2_inode_ops` 的 mock 滞后于内核 `ErrorOr`),会让 ALL 失败,跟本章无关。用 `big_kernel_test` + `run-kernel-test`。

应 `build=0`、测试全绿(0 failed)。日志里能看到 GUI 测试段:`=== cinux::gui SwRaster Tests (F13 §4a) ===`、region / dirty 相关用例。本章新增的 `test_gui_region`(容量坍缩的有界不变量)、`test_gui_dirty`(pump idle 不 flush / 有脏矩形才 flush)、`test_gui_swraster`(Q8.8 alpha 数值精确)都在里面。

## 验收清单

- [ ] core(`third_party/Cinux-GUI/core/`)零内核 include,grep 无输出。
- [ ] `cinux-gui` 能脱离内核独立编译,`ctest` 100% passed(`cinux-gui-smoke`)。
- [ ] 内核侧仅 `host_cinux.cpp` 填 Host 表;`cinux_host_init` 把每个回调指向内核设施。
- [ ] `gui_worker_thread` = `while { pump(); yield(); }`;`set_tick_callback` 全树无人调用(退役)。
- [ ] `big_kernel_test` `build=0` + `run-kernel-test` 全绿(含 test_gui_region/dirty/swraster)。

## 别做这些

- **别**把 `swraster` 当成已经在画屏幕——它目前只是骨架,有单测消费但没接进 `WindowManager::composite()`;真正合成仍走 Canvas。core 经 Host 表驱动的是 input / render_frame / flush 路径,绘制还在内核侧。
- **别**以为 PIT 回调机制被删了——`set_tick_callback`/`invoke_tick_callback` 符号还在(`CINUX_GUI` 下),只是没人注册,实际 no-op。看到它们别误判「反转没做完」。
- **别**给 `HostDesktop.spawn` 当成通用 spawn——它目前无视 `path`/`argv`,直接转去 `create_shell_terminal()`,只有「开 shell」一个动作。
- **别**指望 `init.cpp` 里没有 `#ifdef CINUX_GUI`——那一弧的「§14 收尾」(源码 #ifdef 全归 CMake gate + USB stub)时间上在 xHCI 驱动之后,本章是解耦骨架,worker 线程还内联在 `init.cpp` 里。
- **别** `cmake --build build`(ALL)验证本章——host 单测既有债会失败;用 `big_kernel_test` + `run-kernel-test`。
