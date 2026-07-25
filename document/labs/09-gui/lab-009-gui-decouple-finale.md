---
title: Lab 009 · GUI 解耦收尾(§14 file gate)验证
---

# Lab 009 · GUI 解耦收尾(§14 file gate)验证

> 对应 `document/book/09-gui/009/`。验证档 **B 档**(机制/重构,靠 grep + 读 CMake + 多组合构建验证,无新用户可见功能)。本章把 008 留下的几处源码 `#ifdef CINUX_GUI` / `#ifdef CINUX_USB` 全换成 CMake 的 file gate(同一个接口两份实现文件,CMake 选编一份,调用处零 `#ifdef`)。验证核心三件事——三个核心文件里 `#ifdef` 真的归零了、file gate 在 CMake 里确实选且只选一份、关掉 GUI/USB 时靠空壳文件照样链接通过。

## 目标

确认五件事:

1. `init.cpp` / `main.cpp` / `irq_handlers.cpp` 三个文件里 `#ifdef CINUX_GUI` / `#ifdef CINUX_USB` / `#ifndef CINUX_*` **归零**(§14 成果);
2. 调用处真的是一条直线——`init.cpp` 无条件调 `launch_userspace()` / `usb::init()`,`main.cpp` 无条件调 `handoff_framebuffer_to_gui()`;
3. CMake 的 file gate **选且只选一份**实现(`proc/CMakeLists.txt` 的 launch gate、`drivers/CMakeLists.txt` 的 USB 三层 gate);
4. 关掉 GUI 或 USB 时,**空壳文件顶上、链接照旧通过**(非 GUI 构建、非 GUI+USB 构建的 `big_kernel` 都绿);
5. 默认构建 `big_kernel_test` 0 error + `run-kernel-test` 全绿,且全量 `cmake --build build`(含 host 单测)现在也绿。

## 步骤

### 1. 三个核心文件 #ifdef 归零

```bash
grep -nE '#ifdef CINUX_GUI|#ifdef CINUX_USB|#ifndef CINUX_GUI|#ifndef CINUX_USB|#if defined\(CINUX' \
  kernel/proc/init.cpp kernel/main.cpp kernel/arch/x86_64/irq_handlers.cpp
```

应**无输出**——三个文件各 0 命中。这是 §14 的硬指标:源码级编译开关全赶去 CMake 了。

想看改之前多散,对比 fork 点:

```bash
# 在 CinuxOS 源仓(~/CinuxOS)看 fork 点这三个文件当年有多少处
for f in kernel/proc/init.cpp kernel/main.cpp kernel/arch/x86_64/irq_handlers.cpp; do
  echo "$f: $(git -C ~/CinuxOS show cde30d1:$f 2>/dev/null | grep -cE '#ifdef CINUX|#ifndef CINUX') 处"
done
```

应看到 `init.cpp` 3 处、`main.cpp` 3 处、`irq_handlers.cpp` 1 处,合计 7 → 0。

> **思考**:`test/` 目录里还能 grep 出约 40+ 处 `#ifdef CINUX_HOST_TEST`(如 `test/unit/test_window.cpp:15`)。这不违反 §14——§14 管的是内核**运行源码**里功能开关的调用处,测试基础设施不归它管。`CINUX_HOST_TEST` 是宿主单测开关(选编「只在 host 单测跑的断言」),跟 GUI/USB 这种内核功能开关不是一个层级。同理 `CINUX_LOCKDEP` 是 opt-in 调试开关,不归 §14。

### 2. 调用处是一条直线(零 #ifdef)

```bash
# init.cpp:启动二选一塌成一句
grep -n 'launch_userspace()' kernel/proc/init.cpp
# init.cpp:USB 初始化无条件调
grep -n 'usb::init()' kernel/proc/init.cpp
# main.cpp:帧缓冲交接无条件调
grep -n 'handoff_framebuffer_to_gui' kernel/main.cpp
```

`init.cpp:51` 应是 `launch_userspace();` 一句,前后只有注释(`§14: one interface, two impl files, CMake selects which to link -- no #ifdef here.`)。`init.cpp:58` 应是 `cinux::drivers::usb::init();`。`main.cpp:180` 应是 `cinux::proc::handoff_framebuffer_to_gui(fb, font, console);`。三处都不带任何 `#ifdef`。

### 3. CMake file gate 选且只选一份

```bash
# 启动二选一的 gate(非 GUI 编 shell_launch.cpp;GUI 那份走 gui/ 子目录)
grep -n -A3 'if(NOT CINUX_GUI)' kernel/proc/CMakeLists.txt
# USB 三层 gate
sed -n '34,76p' kernel/drivers/CMakeLists.txt
```

`proc/CMakeLists.txt` 的注释应明说「Exactly one of the two launch_userspace() impls is linked」。`drivers/CMakeLists.txt` 应看到三层:核心传输 `if(CINUX_USB)`、HID 注入 `if(CINUX_USB AND CINUX_GUI)`、空壳 `if(NOT (CINUX_USB AND CINUX_GUI))`,再加上 xHCI IRQ 桩 `if(NOT CINUX_USB)`。

> **思考**:USB 的空壳条件为什么是 `NOT (CINUX_USB AND CINUX_GUI)` 而不是 `NOT CINUX_USB`?——因为「开 USB 关 GUI」时核心传输编了、HID 没编,`usb::init` 符号得有人提供,这时也得链空壳。`NOT (USB AND GUI)` 恰好覆盖「HID 没编」的全部情况。

### 4. 关掉 GUI/USB 时,空壳顶上、链接通过(file gate 的真正价值)

这一步是 file gate 的真正价值:证明关掉可选能力时,靠空壳文件链接器照旧能解析符号。在**独立的 build 目录**里分别建两种非 GUI 配置:

```bash
# 非 GUI + 非 USB:全靠 stub(handoff stub + usb_stub + usb_xhci_stub + mouse_stub)
cmake -S . -B /tmp/build-nogui -DCINUX_GUI=OFF -DCINUX_USB=OFF && \
cmake --build /tmp/build-nogui -j$(nproc) --target big_kernel && echo "nogui-nousb=$?"

# 非 GUI + USB 开:核心传输编、HID 不编、usb_stub 顶上 usb::init
cmake -S . -B /tmp/build-nogui-usb -DCINUX_GUI=OFF -DCINUX_USB=ON && \
cmake --build /tmp/build-nogui-usb -j$(nproc) --target big_kernel && echo "nogui-usb=$?"
```

两个 `echo` 都应是 `=0`。想确认确实是空壳在顶,看链接进来的源里有没有 stub:

```bash
grep -rl 'usb_stub\|mouse_stub\|usb_xhci_stub\|shell_launch' /tmp/build-nogui/CMakeFiles 2>/dev/null | head
```

应看到 `usb_stub.cpp`、`usb_xhci_stub.cpp`、`mouse_stub.cpp`、`shell_launch.cpp` 都被编译了——这些就是 file gate 的「另一份实现」。

### 5. 默认构建 + 测试全绿(含 host 单测)

```bash
cmake --build build --target big_kernel_test -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test 2>&1 | tail -3
cmake --build build --target test_host 2>&1 | grep -E 'passed|failed' | tail -2
```

应 `build=0`、`run-kernel-test` 全绿(0 failed)、`test_host` 全绿。

> **这里和 008 的 lab 不一样**:008 的 lab 叮嘱「别用 `cmake --build build`(ALL),host 单测有既有债会失败」。那笔债(更早 VFS 迁 `ErrorOr` 时漏改的四个 host 单测调用点)现在补齐了,所以全量构建是绿的,`test_host` 能跑。

## 验收清单

- [ ] `init.cpp` / `main.cpp` / `irq_handlers.cpp` 三文件 `#ifdef CINUX_*` grep 无输出(归零);fork 点对比 7 → 0。
- [ ] `init.cpp:51` `launch_userspace()`、`init.cpp:58` `usb::init()`、`main.cpp:180` `handoff_framebuffer_to_gui()` 三处调用零 `#ifdef`。
- [ ] `proc/CMakeLists.txt` launch gate + `drivers/CMakeLists.txt` USB 三层 gate,各选且只选一份实现。
- [ ] 非 GUI 非 USB、非 GUI+USB 两种 `big_kernel` 构建均 `=0`(空壳顶上)。
- [ ] 默认 `big_kernel_test` `build=0` + `run-kernel-test` 全绿 + `test_host` 全绿。

## 别做这些

- **别**以为「关 USB 就该跳过注册 xHCI 中断向量」——`irq_init` 里 `xhci_irq_stub` 是无条件注册进 IDT(vector 0x40)的,哪怕 USB 关。但 USB 关时 MSI-X 不 programming,这个 stub 永不触发,这是「先占 IDT 槽位」的设计。
- **别**把 `usb_stub.cpp` 和 `usb_xhci_stub.cpp` 当成一个文件——前者补 `usb::init()`/`poll_input()`(给 `init.cpp` 调的 C++ 函数),后者补 `extern "C" xhci_irq_handler`(给汇编中断桩调的 C 符号),两个 stub 覆盖不同符号域,gate 条件也不同(`NOT (USB AND GUI)` vs `NOT USB`)。
- **别**以为「非 GUI+USB 时编了 xHCI 核心 = USB 能用」——那时 `usb::init` 是空壳,控制器永远不 bring up,是无害死代码,不是 bug。
- **别**指望 `big_kernel_test` 在非 USB 下能编——`test_xhci.cpp` 无条件进测试源列表且无 `#ifdef CINUX_USB` 守卫,这是测试侧的预存债,本章只保证生产 `big_kernel` 非 USB 兼容。
