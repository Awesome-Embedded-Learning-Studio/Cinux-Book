---
title: 01 · 测试侧的 §14:文件 gate 管不到文件里的那一段
---

# 测试侧的 §14:文件 gate 管不到文件里的那一段

> 054b 把生产代码里的 `#ifdef` 全赶去了 CMake,但留了一笔测试侧的账:`big_kernel_test`(跑在内核里、由 QEMU 驱动的那套测试)一直把 `test_xhci.cpp` 无条件编进源列表,于是关掉 USB 时它引用的 `XHCIController` 等符号找不到,`big_kernel_test` 在非 USB 下编都编不过。054b 把这件事诚实写进了边界。这一章就还这笔账——把同一个 file gate 思路用到测试那一侧,顺手处理一个 054b 没碰到的细节:测试文件**既挂 CMake 文件级 gate,又在文件里留一个 `#ifdef`**,两件事不矛盾,各有各的管辖区。B 档:验证靠构建矩阵——关掉 USB、或开 USB 关 GUI,`big_kernel_test` 都编得过、链接通过。
>
> 一句话定位:054b 讲了 §14 在**生产代码**里的落地;这一章是它在**测试代码**里的落地,外加一个「文件 gate 和文件内 `#ifdef` 什么时候得并存」的细节。

## 这章咱们要点亮什么

1. **测试代码也得守 §14**:`big_kernel_test` 本身就是内核代码,它一样不能靠源码 `#ifdef` 把测试切成两半。
2. **文件 gate 落到测试侧**:`test_xhci.cpp` 整个文件用 CMake 的 `if(CINUX_USB)` 选编,跟 054b 对 USB 驱动干的事一模一样。
3. **什么时候文件 gate 还不够**:`test_xhci.cpp` 同时跨「USB 核心」和「HID 鼠标」两摊,USB 开、GUI 关时整个文件得编(核心测试要跑),但里头那个依赖 `UsbMouse` 的 HID 测试又编不得——这种地方文件内还得留一个 `#ifdef`。

## 病:测试文件无条件进编译列表

先看病。`big_kernel_test` 这个可执行文件在 `kernel/CMakeLists.txt` 里列了一长串测试源文件,其中 `test/test_xhci.cpp` 原来是**无条件**列进去的:

```cmake
add_executable(big_kernel_test
    ...
    test/test_ext2.cpp
    test/test_ahci_write.cpp
    test/test_xhci.cpp          # ← 无条件,关 USB 时它照样编
    test/test_ext2_allocator.cpp
    ...
)
```

`test_xhci.cpp` 里调的是 `XHCIController`、`usb_descriptor` 这些——它们全在 `if(CINUX_USB)` gate 后头(054b 干的)。所以一旦 `CINUX_USB=OFF`,这些驱动的 `.cpp` 不编,`test_xhci.cpp` 却还在引用它们的符号,链接器报 undefined。结果就是:`big_kernel_test` 在非 USB 配置下编不过。这就是 054b 写进边界的那笔债。

## 治:给测试文件也挂 CMake 文件 gate

治法跟 054b 对生产代码干的事一一对应——把 `test_xhci.cpp` 从那个无条件列表里摘出来,改用 `if(CINUX_USB)` 选编:

```cmake
# F5-M5 xHCI: gate the USB test on CINUX_USB so a CINUX_USB=OFF build links
# (the driver is absent; run_xhci_tests is #ifdef CINUX_USB'd in main_test.cpp).
if(CINUX_USB)
    target_sources(big_kernel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/test_xhci.cpp)
endif()
```

（`kernel/CMakeLists.txt:272` 起。)关 USB 时这个 `if` 整块不执行,`test_xhci.cpp` 根本不进编译,自然没有悬空引用——`big_kernel_test` 在非 USB 下链接通过。

`test_xhci.cpp` 不编了,里头的入口函数 `run_xhci_tests()` 也就没人定义了。它在 `main_test.cpp` 里被调用,所以调用那一头也得挂同一个开关——不然 `main_test.cpp` 引用一个不存在的符号,又断。两头一起 gate:

```cpp
#ifdef CINUX_USB
void run_xhci_tests();
#endif
...
#ifdef CINUX_USB
    // xHCI tests (F5-M5): PCI find + BAR0 map + reset. ...
    run_xhci_tests();
#endif
```

（`kernel/test/main_test.cpp:102` 的声明、`:270` 的调用。）到这儿,非 USB 的 `big_kernel_test` 就能编能链了。

## 细节:为什么测试文件里还留着一个 #ifdef

注意上面 `main_test.cpp` 里用了 `#ifdef CINUX_USB`——这不是又把 §14 违反回去了吗?这里是「文件 gate 不够用、文件内还得补一个」的典型场景,值得单独说。

`test_xhci.cpp` 这个文件**跨了两摊东西**:一摊是 xHCI **核心传输**测试(找控制器、reset、address device),只依赖 `XHCIController`,USB 开就能跑;另一摊是 **HID 鼠标**测试(`test_hid_mouse`),它要实例化一个 `UsbMouse`(`UsbMouse mouse;` 再 `bind(slot)`、`init(...)`)——而 `UsbMouse` 是 HID,在 054b 的双 gate 里是 `if(CINUX_USB AND CINUX_GUI)` 才编的。

于是冒出一个中间配置:**USB 开、GUI 关**。这时 `test_xhci.cpp` 整个文件得编(因为 USB 开,`if(CINUX_USB)` 进了它,核心测试要跑),但 `UsbMouse` 没编(GUI 关)。如果 `test_hid_mouse` 不挂守卫,它引用的 `UsbMouse` vtable 就是 undefined,链接断。

文件 gate 管的是「整个文件编不编」,管不到「文件里某一段编不编」。所以这个 HID 测试得在**文件内**再挂一个 `#ifdef CINUX_GUI`:

```cpp
// HID mouse bring-up needs UsbMouse (compiled only under CINUX_USB AND CINUX_GUI,
// see drivers/CMakeLists.txt).  Gate the whole test + its RUN_TEST on CINUX_GUI
// so a USB=ON+GUI=OFF build keeps the xHCI core tests without an unresolved
// UsbMouse vtable.
#ifdef CINUX_GUI
void test_hid_mouse() {
    ...
}
#endif
```

（`kernel/test/test_xhci.cpp:200`,连同文件头 `usb_mouse.hpp` 的 include(`:20`)和 `run_xhci_tests` 里那条 `RUN_TEST`(`:290`)一起 gate。）这跟 054b 里 USB 驱动拆「核心传输 / HID 注入」双 gate 是**同一个形状**——只不过 054b 是拆成两个 CMake gate(核心一个文件、HID 另一个文件),这里因为核心和 HID 测试挤在同一个 `test_xhci.cpp` 里,只好用「文件 gate + 文件内 `#ifdef`」的组合来表达同一件事。

> 所以「源码里绝对不能有 `#ifdef`」是个简化说法,精确点是:**能靠 CMake 文件 gate 解决的,就别用源码 `#ifdef`**。只有当一个文件**横跨两个开关**(像 `test_xhci.cpp` 横跨 USB 和 GUI),又没法把它再拆成两个文件时,文件内留 `#ifdef` 才是合理的——而且得像这里一样,注释把「为什么这个 `#ifdef` 躲不掉」写清楚。

## 诚实的边界

这套做完,§14 在 GUI/USB 上就收齐了:生产代码(054b)和测试代码(本章)都不再靠源码 `#ifdef` 把路径切成两半,关掉 GUI 或 USB 时各有一组空壳/gate 顶着,四种构建组合全能编能链。

**host 单测(`test/unit/`)**是另一回事,不归 §14 管。它们跑在宿主机上、不进 `big_kernel_test`,条件编译用的是 `CINUX_HOST_TEST`(宿主单测开关),跟 GUI/USB 这种内核功能开关不是一个层级——054b 讲过。

验证该看到什么,见配套 lab。下一章(056)换条线,去安全卷开 NX/SMEP/SMAP 和 ASLR。
