---
title: Lab 010 · GUI 解耦收尾(测试侧)验证
---

# Lab 010 · GUI 解耦收尾(测试侧)验证

> 对应 `document/book/09-gui/010/`。验证档 **B 档**(机制/重构,靠构建矩阵 + grep 验证)。本章把 009 的 file gate 思路用到测试侧:`big_kernel_test` 里 `test_xhci.cpp` 不再无条件编译,改挂 `if(CINUX_USB)`,并在文件内给依赖 `UsbMouse` 的 HID 测试补一个 `#ifdef CINUX_GUI`。验证核心三件事——测试文件真挂了 CMake gate、关 USB 时 `big_kernel_test` 编得过(009 标的那笔债还了)、USB 开 GUI 关时核心测试还在跑但 HID 测试自动退场。

## 目标

确认四件事:

1. `test_xhci.cpp` 在 `kernel/CMakeLists.txt` 里挂 `if(CINUX_USB)` 选编,不在无条件列表里;
2. **非 USB** 的 `big_kernel_test` 编得过、链接通过(009 边界里那笔债);
3. **USB 开 / GUI 关** 的 `big_kernel_test` 也编得过——`test_xhci.cpp` 编了,但 `test_hid_mouse` 因 `#ifdef CINUX_GUI` 自动退场;
4. 默认配置 `run-kernel-test` 不回归(931 passed)。

## 步骤

### 1. 测试文件挂 CMake 文件 gate

```bash
# test_xhci.cpp 应只出现在 if(CINUX_USB) 块里,不在无条件 add_executable 列表里
grep -n 'test_xhci.cpp' kernel/CMakeLists.txt
```

应只在 `if(CINUX_USB)` 那块(约 `:273`)命中**一次**,`add_executable(big_kernel_test ...)` 的无条件列表里**没有**它。再确认 `main_test.cpp` 两头都挂了守卫:

```bash
grep -n -B1 'run_xhci_tests' kernel/test/main_test.cpp
```

应看到声明(`:102`)和调用(`:270`)各自前一行的 `#ifdef CINUX_USB`(`:101`、`:266`)。

### 2. 非 USB 的 big_kernel_test 编得过(还 009 的债)

009 的 lab 特意只用 `big_kernel` 而非 `big_kernel_test` 验证非 USB——因为那时 `big_kernel_test` 在非 USB 下编不过。现在补上了,直接编它:

```bash
cmake -S . -B /tmp/build-nousb-bkt -DCINUX_USB=OFF && \
cmake --build /tmp/build-nousb-bkt -j$(nproc) --target big_kernel_test && echo "build=$?"
```

应 `build=0`。`test_xhci.cpp` 这时根本没编(`if(CINUX_USB)` 整块跳过),所以没有悬空的 `XHCIController` 引用。再补一格矩阵——GUI 也关掉:

```bash
cmake -S . -B /tmp/build-off-bkt -DCINUX_GUI=OFF -DCINUX_USB=OFF && \
cmake --build /tmp/build-off-bkt -j$(nproc) --target big_kernel_test && echo "build=$?"
```

同样 `build=0`。`test_xhci.cpp` 是唯一的 USB 专属测试文件,USB 一关它整文件不进编译,`test_hid_mouse` 也连带消失,所以 GUI 关不关都不影响。到这儿四种组合(全开 / USB 关 / USB 开 GUI 关 / 都关)的 `big_kernel_test` 都验过了。

> **对比 009 的 lab**:那条「`big_kernel_test` 在非 USB 下还是链接不过,留后续」的边界——到这里作废了。同一套 file gate 思路,生产侧(009)和测试侧(本章)都收齐。

### 3. USB 开 / GUI 关:核心测试在跑,HID 测试退场

这是「文件 gate + 文件内 `#ifdef`」并存的那个中间配置。USB 开 → `test_xhci.cpp` 整个编;GUI 关 → `test_hid_mouse` 被 `#ifdef CINUX_GUI` 摘掉。

```bash
cmake -S . -B /tmp/build-usb-nogui-bkt -DCINUX_GUI=OFF -DCINUX_USB=ON && \
cmake --build /tmp/build-usb-nogui-bkt -j$(nproc) --target big_kernel_test && echo "build=$?"
```

应 `build=0`。确认 `test_xhci.cpp` 确实编了(否则 gate 逻辑反了):

```bash
grep -rl 'test_xhci.cpp' /tmp/build-usb-nogui-bkt/kernel/CMakeFiles 2>/dev/null | head -1
```

应有命中(`test_xhci.cpp` 在 USB 开时进编译)。再去源码看那个文件内 `#ifdef`:

```bash
grep -n -B2 'void test_hid_mouse\|RUN_TEST(test_xhci::test_hid_mouse' kernel/test/test_xhci.cpp
```

应看到 `test_hid_mouse` 定义(`:201`)和它的 `RUN_TEST`(`:290`)各自前一行的 `#ifdef CINUX_GUI`(`:200`、`:289`),还有文件头 `usb_mouse.hpp` 的 include 也挂在 `#ifdef CINUX_GUI`(`:19`)下。这就是「文件 gate 管不到文件内某一段,得补 `#ifdef`」的现场。

> **思考**:为什么这个 `#ifdef` 不算违反 §14?——因为 `test_xhci.cpp` 横跨 USB 核心和 HID 鼠标两摊,文件 gate 只能决定整个文件编不编;USB 开 GUI 关时文件得编(核心测试要跑),但 HID 测试依赖的 `UsbMouse` 没编,只能在文件内挂守卫。能靠 CMake 文件 gate 解决就别用 `#ifdef`;只有横跨两个开关、又没法再拆文件时,文件内 `#ifdef` 才合理,且注释得说清为什么躲不掉。

### 4. 默认配置不回归

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -E '=== Tests:|ALL TESTS'
```

应是 `931 passed, 0 failed` + `ALL TESTS PASSED`。默认 USB+GUI 都开,`test_xhci.cpp` 照编、`run_xhci_tests()` 照调(没 qemu-xhci 硬件时那几个用例 skip 即 pass),跟补 gate 前行为一致。

## 验收清单

- [ ] `test_xhci.cpp` 只在 `if(CINUX_USB)` 块里,不在 `big_kernel_test` 无条件列表;`main_test.cpp` 声明+调用两头都挂 `#ifdef CINUX_USB`。
- [ ] 非 USB `big_kernel_test` `build=0`(009 边界的债还了)。
- [ ] USB 开 GUI 关 `big_kernel_test` `build=0`,`test_xhci.cpp` 编了,`test_hid_mouse` 被 `#ifdef CINUX_GUI` 摘掉。
- [ ] 默认 `run-kernel-test` 931 passed,0 failed。

## 别做这些

- **别**以为「文件 gate 和文件内 `#ifdef` 并存」是没收干净——`test_xhci.cpp` 横跨 USB 核心 + HID 鼠标两个开关,文件 gate 管整个文件、文件内 `#ifdef` 管 HID 那一段,两层各管各的,注释里写清了为什么。
- **别**拿 §14 的尺子去量 `test/unit/`(host 单测)——那套用 `CINUX_HOST_TEST`(宿主单测开关),跑在宿主机上、不进 `big_kernel_test`,跟内核功能开关不是一个层级(009 讲过)。
- **别**指望非 USB 的 `run-kernel-test` 能验 xHCI——那套测试在非 USB 下整个不编(`test_xhci.cpp` 被 gate 掉);验 xHCI 用默认配置的 `run-kernel-test-xhci`(挂 qemu-xhci,见 011)。
